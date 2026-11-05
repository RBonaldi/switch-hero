// The stage behind the highway. See stage.hpp.
//
// A small low-poly 3D scene, rebuilt every frame: meshes are generated on the
// CPU, lit per vertex (coloured key and fill lights plus a rim from the LED
// wall), depth sorted and drawn back to front through look::mesh(), so it
// costs one batched draw and works the same on desktop and Switch.
//
// The band is original to this game: Nina on vocals (platinum crop, green
// bomber), Caio on guitar (the player: long dark hair, leather jacket, a teal
// offset guitar), Luna on bass (violet bob, white jacket) and Bento on drums
// (beanie and beard). Bodies are capsules on a simple rig; hands are placed by
// two-bone IK on the pick, the neck, the mic or a drum, and the elbows follow.
#include "stage.hpp"
#include "look.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace fret::stage {
using namespace look;
namespace {
constexpr float Pi = 3.14159265f;

// ------------------------------------------------------------ maths
struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(V3 a) { return std::sqrt(dot(a, a)); }
V3 norm(V3 a) {
    const float l = length(a);
    return l > 1e-6f ? a * (1 / l) : V3{0, 1, 0};
}
V3 lerp(V3 a, V3 b, float t) { return a + (b - a) * t; }
float frac(double v) { return float(v - std::floor(v)); }
float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
float ease(double age, double life) {
    if (age < 0 || age > life)
        return 0;
    const float t = float(1 - age / life);
    return t * t;
}
float smooth(float t) {
    t = clamp01(t);
    return t * t * (3 - 2 * t);
}
SDL_Color rgb(int r, int g, int b, int a = 255) { return {Uint8(r), Uint8(g), Uint8(b), Uint8(a)}; }
// An orthonormal frame: r(ight), u(p), f(orward, toward the audience).
struct Frame {
    V3 r{1, 0, 0}, u{0, 1, 0}, f{0, 0, 1};
    V3 at(V3 o, float x, float y, float z) const { return o + r * x + u * y + f * z; }
    V3 dir(float x, float y, float z) const { return r * x + u * y + f * z; }
};
Frame frameFrom(V3 up, V3 forwardHint) {
    Frame fr;
    fr.u = norm(up);
    fr.r = norm(cross(fr.u, forwardHint));
    fr.f = cross(fr.r, fr.u);
    return fr;
}
// Two-bone IK in 3D: the middle joint, bent toward `pole`.
V3 ik(V3 root, V3 target, float l1, float l2, V3 pole) {
    V3 d = target - root;
    const float dist = std::clamp(length(d), .001f, l1 + l2 - .002f);
    const V3 u = norm(d);
    const float a = (l1 * l1 - l2 * l2 + dist * dist) / (2 * dist);
    const float h = std::sqrt(std::max(0.0f, l1 * l1 - a * a));
    V3 p = pole - u * dot(pole, u);
    if (length(p) < 1e-4f)
        p = {0, -1, 0};
    return root + u * a + norm(p) * h;
}

// ------------------------------------------------------------ camera, lights, triangles
struct Camera {
    V3 pos, right, up, fwd;
    float focal = 772, cx = 640, cy = 300;
    void look(V3 from, V3 to) {
        pos = from;
        fwd = norm(to - from);
        right = norm(cross(fwd, {0, 1, 0}));
        up = cross(right, fwd);
    }
    V3 view(V3 p) const {
        const V3 d = p - pos;
        return {dot(d, right), dot(d, up), dot(d, fwd)};
    }
    SDL_FPoint screen(V3 v) const { return {cx + focal * v.x / v.z, cy - focal * v.y / v.z}; }
} cam;

struct Lighting {
    V3 keyDir, fillDir;          // direction *toward* each light
    V3 key, fill, rim, ambient;  // colours as 0..1 floats
} lit;
V3 col3(SDL_Color c) { return {c.r / 255.f, c.g / 255.f, c.b / 255.f}; }

SDL_Color shadeVertex(V3 base, V3 n, V3 p) {
    const V3 v = norm(cam.pos - p);
    const float kd = std::max(0.0f, dot(n, lit.keyDir)), fd = std::max(0.0f, dot(n, lit.fillDir));
    const float rim = std::pow(1 - std::max(0.0f, dot(n, v)), 3.0f);
    V3 c = {base.x * (lit.ambient.x + lit.key.x * kd + lit.fill.x * fd),
            base.y * (lit.ambient.y + lit.key.y * kd + lit.fill.y * fd),
            base.z * (lit.ambient.z + lit.key.z * kd + lit.fill.z * fd)};
    c = c + lit.rim * (rim * .9f);
    return rgb(int(std::min(1.0f, c.x) * 255), int(std::min(1.0f, c.y) * 255), int(std::min(1.0f, c.z) * 255));
}

struct Tri {
    SDL_Vertex v[3];
    float depth;
};
std::vector<Tri> tris;      // sorted each frame
std::vector<SDL_Vertex> vbuf;
std::vector<int> ibuf;
bool emissive = false;      // while true, colours are used as they are

// One triangle in world space with per-vertex normals; back faces are culled.
void tri(V3 a, V3 b, V3 c, V3 na, V3 nb, V3 nc, V3 base) {
    V3 face = cross(b - a, c - a);
    const V3 hint = na + nb + nc;
    if (dot(face, hint) < 0)
        face = face * -1;
    const V3 centre = (a + b + c) * (1.f / 3);
    if (dot(face, cam.pos - centre) <= 0)
        return; // facing away
    const V3 va = cam.view(a), vb = cam.view(b), vc = cam.view(c);
    if (va.z < .2f || vb.z < .2f || vc.z < .2f)
        return;
    Tri t;
    const V3 ps[3] = {va, vb, vc}, ws[3] = {a, b, c}, ns[3] = {na, nb, nc};
    for (int i = 0; i < 3; ++i) {
        t.v[i].position = cam.screen(ps[i]);
        t.v[i].color = emissive ? rgb(int(clamp01(base.x) * 255), int(clamp01(base.y) * 255), int(clamp01(base.z) * 255))
                                : shadeVertex(base, norm(ns[i]), ws[i]);
        t.v[i].tex_coord = {0, 0};
    }
    t.depth = std::max({va.z, vb.z, vc.z}) * .5f + (va.z + vb.z + vc.z) / 6;
    tris.push_back(t);
}
void flatTri(V3 a, V3 b, V3 c, V3 n, V3 base) { tri(a, b, c, n, n, n, base); }
void flatQuad(V3 a, V3 b, V3 c, V3 d, V3 n, V3 base) {
    flatTri(a, b, c, n, base);
    flatTri(a, c, d, n, base);
}
// Draws everything queued so far, far to near, and empties the queue.
void flushTris() {
    std::sort(tris.begin(), tris.end(), [](const Tri &a, const Tri &b) { return a.depth > b.depth; });
    vbuf.clear(), ibuf.clear();
    for (const auto &t : tris) {
        const int base = int(vbuf.size());
        vbuf.insert(vbuf.end(), t.v, t.v + 3);
        ibuf.insert(ibuf.end(), {base, base + 1, base + 2});
    }
    if (!vbuf.empty())
        mesh(nullptr, vbuf, ibuf);
    tris.clear();
}

// ------------------------------------------------------------ mesh builders
// A box centred on `c` with half sizes along the frame's axes.
void box(V3 c, const Frame &fr, float hx, float hy, float hz, V3 col) {
    auto P = [&](float x, float y, float z) { return fr.at(c, x * hx, y * hy, z * hz); };
    flatQuad(P(-1, -1, 1), P(1, -1, 1), P(1, 1, 1), P(-1, 1, 1), fr.f, col);
    flatQuad(P(1, -1, -1), P(-1, -1, -1), P(-1, 1, -1), P(1, 1, -1), fr.f * -1, col);
    flatQuad(P(1, -1, 1), P(1, -1, -1), P(1, 1, -1), P(1, 1, 1), fr.r, col);
    flatQuad(P(-1, -1, -1), P(-1, -1, 1), P(-1, 1, 1), P(-1, 1, -1), fr.r * -1, col);
    flatQuad(P(-1, 1, 1), P(1, 1, 1), P(1, 1, -1), P(-1, 1, -1), fr.u, col);
    flatQuad(P(-1, -1, -1), P(1, -1, -1), P(1, -1, 1), P(-1, -1, 1), fr.u * -1, col);
}
void box(V3 c, float hx, float hy, float hz, V3 col) { box(c, Frame{}, hx, hy, hz, col); }
// A box whose bottom and top faces differ in size (a tapered trunk).
void frustum(V3 bottom, V3 top, V3 fwd, float bw, float bd, float tw, float td, V3 col) {
    const Frame fr = frameFrom(top - bottom, fwd);
    V3 b[4] = {fr.at(bottom, -bw, 0, bd), fr.at(bottom, bw, 0, bd), fr.at(bottom, bw, 0, -bd), fr.at(bottom, -bw, 0, -bd)};
    V3 t[4] = {fr.at(top, -tw, 0, td), fr.at(top, tw, 0, td), fr.at(top, tw, 0, -td), fr.at(top, -tw, 0, -td)};
    const V3 n[4] = {fr.f, fr.r, fr.f * -1, fr.r * -1};
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        flatQuad(b[i], b[j], t[j], t[i], n[i], col);
    }
    flatQuad(t[0], t[1], t[2], t[3], fr.u, col);
    flatQuad(b[3], b[2], b[1], b[0], fr.u * -1, col);
}
// A tapered cylinder between two points, smooth shaded, with caps.
void cylinder(V3 a, V3 b, float ra, float rb, V3 col, int sides = 8, bool caps = true) {
    const V3 axis = norm(b - a);
    const V3 side = std::abs(axis.y) < .9f ? V3{0, 1, 0} : V3{1, 0, 0};
    const V3 e1 = norm(cross(axis, side)), e2 = cross(axis, e1);
    for (int i = 0; i < sides; ++i) {
        const float t0 = 2 * Pi * i / sides, t1 = 2 * Pi * (i + 1) / sides;
        const V3 n0 = e1 * std::cos(t0) + e2 * std::sin(t0), n1 = e1 * std::cos(t1) + e2 * std::sin(t1);
        const V3 a0 = a + n0 * ra, a1 = a + n1 * ra, b0 = b + n0 * rb, b1 = b + n1 * rb;
        tri(a0, a1, b1, n0, n1, n1, col);
        tri(a0, b1, b0, n0, n1, n0, col);
        if (caps) {
            flatTri(b, b0, b1, axis, col);
            flatTri(a, a1, a0, axis * -1, col);
        }
    }
}
// An ellipsoid in a frame, smooth shaded.
void ellipsoid(V3 c, const Frame &fr, float rx, float ry, float rz, V3 col, int slices = 10, int stacks = 6,
               float fromLat = -Pi / 2) {
    auto P = [&](float lat, float lon, V3 &n) {
        const float x = std::cos(lat) * std::sin(lon), y = std::sin(lat), z = std::cos(lat) * std::cos(lon);
        n = fr.dir(x / rx, y / ry, z / rz);
        return fr.at(c, x * rx, y * ry, z * rz);
    };
    for (int i = 0; i < stacks; ++i) {
        const float la0 = fromLat + (Pi / 2 - fromLat) * i / stacks, la1 = fromLat + (Pi / 2 - fromLat) * (i + 1) / stacks;
        for (int j = 0; j < slices; ++j) {
            const float lo0 = 2 * Pi * j / slices, lo1 = 2 * Pi * (j + 1) / slices;
            V3 n00, n01, n10, n11;
            const V3 p00 = P(la0, lo0, n00), p01 = P(la0, lo1, n01), p10 = P(la1, lo0, n10), p11 = P(la1, lo1, n11);
            tri(p00, p01, p11, n00, n01, n11, col);
            tri(p00, p11, p10, n00, n11, n10, col);
        }
    }
}
void sphere(V3 c, float r, V3 col, int slices = 8, int stacks = 5) { ellipsoid(c, Frame{}, r, r, r, col, slices, stacks); }
// A star-shaped outline r(angle) extruded `depth` along the frame's f axis.
template <class Outline>
void slab(V3 c, const Frame &fr, Outline r, int steps, float depth, V3 col, V3 rimCol) {
    std::vector<V3> front, back;
    for (int i = 0; i < steps; ++i) {
        const float a = 2 * Pi * i / steps;
        const float rad = r(a);
        const float x = std::cos(a) * rad, y = std::sin(a) * rad;
        front.push_back(fr.at(c, x, y, depth / 2));
        back.push_back(fr.at(c, x, y, -depth / 2));
    }
    const V3 cf = fr.at(c, 0, 0, depth / 2), cb = fr.at(c, 0, 0, -depth / 2);
    for (int i = 0; i < steps; ++i) {
        const int j = (i + 1) % steps;
        flatTri(cf, front[size_t(i)], front[size_t(j)], fr.f, col);
        flatTri(cb, back[size_t(j)], back[size_t(i)], fr.f * -1, col);
        const V3 out = norm((front[size_t(i)] + front[size_t(j)]) * .5f - cf);
        flatQuad(front[size_t(i)], back[size_t(i)], back[size_t(j)], front[size_t(j)], out, rimCol);
    }
}

// ------------------------------------------------------------ memory of the show
struct Memory {
    int hits = 0, misses = 0;
    double lastHit = -1e9, lastMiss = -1e9, powerStart = -1e9, lastTime = -1e9;
    bool power = false;
} mem;

struct Show { // what everyone reacts to this frame
    double time, beat;
    float energy, ph, hit, miss;
    bool power, failed, danger;
    uint8_t held;
};

// How each venue dresses the stage. Index 8 is the Inferno, 9 to 11 the venues
// past the stadium, 12 the Quickplay club.
struct VenueLook {
    SDL_Color keyA, keyB, fillA, fillB; // light palettes the show drifts between
    int led;        // 0 none, 1 small screen, 2 full wall, 3 full wall and side screens
    int lamps;      // lamps on the truss (0: bare bulbs hanging from the ceiling)
    int backdrop;   // 0 brick, 1 black wall, 2 open sky at sunset, 3 a cavern of lava,
                    // 4 an ice field under an aurora, 5 open space with a planet
    float crowd;    // how full the pit is, 0..1
    float level;    // overall brightness
};
const VenueLook venueLooks[13] = {
    {{255, 190, 110, 255}, {255, 150, 80, 255}, {90, 70, 50, 255}, {70, 60, 60, 255}, 0, 0, 0, .25f, .8f},     // garage
    {{255, 170, 70, 255}, {60, 200, 190, 255}, {120, 70, 30, 255}, {30, 100, 100, 255}, 1, 3, 0, .45f, .9f}, // tavern
    {{140, 230, 80, 255}, {250, 220, 60, 255}, {40, 90, 40, 255}, {90, 80, 30, 255}, 1, 4, 1, .6f, 1},        // campus
    {{245, 80, 180, 255}, {70, 220, 255, 255}, {110, 30, 90, 255}, {30, 90, 140, 255}, 2, 5, 1, .75f, 1},    // neon club
    {{255, 60, 40, 255}, {255, 150, 40, 255}, {120, 20, 20, 255}, {110, 60, 20, 255}, 2, 5, 0, .85f, .9f},   // underground
    {{255, 160, 80, 255}, {200, 90, 220, 255}, {150, 80, 60, 255}, {90, 50, 130, 255}, 2, 5, 2, 1, 1},       // sunset festival
    {{110, 180, 255, 255}, {240, 240, 255, 255}, {40, 70, 160, 255}, {90, 90, 130, 255}, 3, 7, 1, 1, 1.05f}, // arena
    {{255, 200, 90, 255}, {255, 255, 235, 255}, {140, 100, 40, 255}, {120, 120, 110, 255}, 3, 9, 2, 1, 1.1f}, // stadium
    {{255, 60, 20, 255}, {255, 150, 30, 255}, {190, 20, 10, 255}, {160, 50, 10, 255}, 2, 6, 3, 1, 1.15f},      // the Inferno
    {{255, 150, 70, 255}, {40, 210, 190, 255}, {150, 70, 40, 255}, {20, 110, 120, 255}, 3, 7, 2, 1, 1.05f},    // harbour barge
    {{130, 230, 255, 255}, {120, 255, 170, 255}, {40, 90, 150, 255}, {30, 130, 100, 255}, 3, 8, 4, 1, 1.1f},   // glacier festival
    {{150, 120, 255, 255}, {120, 255, 90, 255}, {60, 40, 150, 255}, {40, 120, 60, 255}, 3, 9, 5, 1, 1.1f},     // orbital arena
    {{255, 176, 90, 255}, {245, 80, 180, 255}, {80, 120, 255, 255}, {60, 200, 190, 255}, 2, 5, 1, .8f, 1},    // quickplay club
};
const VenueLook *vl = &venueLooks[12];
V3 hexColor(unsigned c) { return {((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f}; }
V3 guitarFinish{.12f, .62f, .6f}, jacketColour{.1f, .1f, .11f};
bool pyroOn = false;
int rivalOn = -1;
V3 rivalSpot{-2.05f, 0, .5f};
int outcome = 0; // after a duel: 1 the player won, -1 the rival won
V3 rivalColour{1, 1, 1};
float rivalHurt = 0, rivalStrike = 0;

// ------------------------------------------------------------ people
struct Style {
    V3 skin, top, jacket, pants, shoes, hair;
    int hairStyle; // 0 platinum crop, 1 long, 2 beanie and beard, 3 bob
    bool sleeves;  // jacket sleeves down to the wrist
};
struct Rig {
    Frame body, head;
    V3 pelvis, chest, neck, headC, shoulderL, shoulderR, hipL, hipR, footL, footR;
};
// root = point between the feet; yaw turns toward screen right; lean tips the
// spine toward the audience; nod tips the head; crouch bends the knees.
Rig rig(V3 root, float yaw, float lean, float roll, float crouch, float nod, float stance) {
    Rig k;
    const V3 F = {std::sin(yaw), 0, std::cos(yaw)}, R = {std::cos(yaw), 0, -std::sin(yaw)};
    k.pelvis = root + V3{0, .95f - crouch, 0};
    V3 up = norm(V3{0, std::cos(lean), 0} + F * std::sin(lean) + R * std::sin(roll));
    k.body = frameFrom(up, F);
    k.chest = k.pelvis + k.body.u * .32f;
    k.neck = k.pelvis + k.body.u * .55f;
    k.head = frameFrom(norm(k.body.u * std::cos(nod) + k.body.f * std::sin(nod)), k.body.f);
    k.headC = k.neck + k.head.u * .15f;
    const V3 shoulders = k.pelvis + k.body.u * .5f;
    k.shoulderL = shoulders - k.body.r * .2f; // screen left: the character's right side
    k.shoulderR = shoulders + k.body.r * .2f;
    k.hipL = k.pelvis - R * .1f, k.hipR = k.pelvis + R * .1f;
    k.footL = root - R * stance + F * .05f, k.footR = root + R * stance - F * .05f;
    return k;
}
void legs(const Rig &k, const Style &s) {
    for (int side : {-1, 1}) {
        const V3 hip = side < 0 ? k.hipL : k.hipR, foot = side < 0 ? k.footL : k.footR;
        const V3 ankle = foot + V3{0, .08f, 0};
        const V3 knee = ik(hip, ankle, .44f, .44f, k.body.f + k.body.r * (side * .4f));
        cylinder(hip, knee, .09f, .075f, s.pants);
        cylinder(knee, ankle, .072f, .058f, s.pants);
        box(foot + V3{0, .055f, .04f}, frameFrom({0, 1, 0}, k.body.f), .06f, .055f, .12f, s.shoes);
    }
}
void trunk(const Rig &k, const Style &s) {
    box(k.pelvis, k.body, .17f, .09f, .11f, s.pants);
    frustum(k.pelvis + k.body.u * .06f, k.pelvis + k.body.u * .5f, k.body.f, .15f, .1f, .21f, .12f, s.top);
    // An open jacket: two panels over the shirt.
    for (int side : {-1, 1}) {
        const V3 b = k.pelvis + k.body.u * .07f + k.body.r * (side * .12f) + k.body.f * .02f;
        const V3 t = k.pelvis + k.body.u * .5f + k.body.r * (side * .16f) + k.body.f * .02f;
        frustum(b, t, k.body.f, .055f, .1f, .07f, .12f, s.jacket);
    }
    cylinder(k.neck - k.body.u * .04f, k.headC - k.head.u * .06f, .05f, .045f, s.skin, 6);
}
void head(const Rig &k, const Style &s, float swing) {
    const Frame &h = k.head;
    const V3 c = k.headC;
    ellipsoid(c, h, .1f, .125f, .11f, s.skin);
    box(h.at(c, 0, .01f, .1f), h, .085f, .02f, .02f, {.05f, .05f, .07f}); // dark glasses
    box(h.at(c, 0, -.04f, .11f), h, .015f, .025f, .02f, s.skin);          // nose
    switch (s.hairStyle) {
    case 0: { // platinum crop with a few spikes
        ellipsoid(h.at(c, 0, .03f, -.01f), h, .108f, .11f, .118f, s.hair, 10, 4, 0);
        for (int i = 0; i < 5; ++i) {
            const float a = (i - 2) * .35f;
            const V3 base = h.at(c, std::sin(a) * .07f, .1f, -.02f + std::cos(a) * .03f);
            cylinder(base, base + h.dir(std::sin(a) * .05f, .07f, -.05f + swing * .02f), .03f, .004f, s.hair, 4, false);
        }
        break;
    }
    case 1: { // long hair: a cap and a curtain that flies with the headbang
        ellipsoid(h.at(c, 0, .025f, -.01f), h, .112f, .115f, .12f, s.hair, 10, 4, 0);
        const V3 back = h.at(c, 0, -.02f, -.07f);
        for (int side : {-1, 1}) {
            const V3 top = h.at(c, side * .095f, .02f, -.01f);
            const V3 tip = top + V3{side * .02f + swing * .12f, -.32f + std::abs(swing) * .1f, .05f - swing * .02f};
            frustum(top, tip, h.f, .02f, .05f, .03f, .03f, s.hair);
        }
        frustum(back, back + V3{swing * .1f, -.34f, -.03f}, h.f * -1, .1f, .03f, .12f, .02f, s.hair);
        break;
    }
    case 2: { // beanie and a beard
        ellipsoid(h.at(c, 0, .04f, -.01f), h, .112f, .12f, .12f, s.hair, 10, 4, 0);
        cylinder(h.at(c, 0, .03f, -.01f), h.at(c, 0, .07f, -.01f), .118f, .118f, s.hair * .7f, 10);
        box(h.at(c, 0, -.09f, .06f), h, .085f, .055f, .06f, {.2f, .12f, .07f});
        break;
    }
    default: { // bob cut
        ellipsoid(h.at(c, 0, .025f, -.01f), h, .115f, .12f, .122f, s.hair, 10, 4, 0);
        for (int side : {-1, 1})
            box(h.at(c, side * .1f, -.05f, -.01f), h, .025f, .08f, .09f, s.hair);
        box(h.at(c, 0, -.04f, -.09f), h, .1f, .09f, .03f, s.hair);
    }
    }
}
void arm(V3 shoulder, V3 hand, V3 pole, const Style &s) {
    const V3 elbow = ik(shoulder, hand, .29f, .27f, pole);
    cylinder(shoulder, elbow, .062f, .052f, s.sleeves ? s.jacket : s.skin);
    cylinder(elbow, hand, .048f, .04f, s.sleeves ? s.jacket : s.skin);
    sphere(hand, .045f, s.skin, 6, 4);
}

// A guitar or bass on the body, neck rising toward screen right.
struct Axe {
    V3 bodyC, along, across, face;
};
Axe strap(const Rig &k, float neckUp) {
    Axe g;
    g.face = k.body.f;
    g.along = norm(k.body.r * std::cos(neckUp) + k.body.u * std::sin(neckUp));
    g.across = cross(g.face, g.along);
    g.bodyC = k.pelvis + k.body.f * .16f - k.body.r * .08f + k.body.u * .04f;
    return g;
}
void axe(const Axe &g, float neckLen, V3 finish, bool bass) {
    Frame fr;
    fr.r = g.along, fr.u = g.across, fr.f = g.face;
    // An original offset outline: a big lower bout, a smaller upper horn.
    auto outline = [bass](float a) {
        const float horn = std::max(0.0f, std::cos(a - 1.25f));
        const float lower = std::max(0.0f, std::cos(a - 3.6f));
        return (bass ? .2f : .18f) * (1 + .3f * std::pow(horn, 6.0f) + .2f * lower - .12f * std::cos(2 * a));
    };
    slab(fr.at(g.bodyC, -.04f, 0, 0), fr, outline, 28, .05f, finish, finish * .55f);
    slab(fr.at(g.bodyC, -.05f, -.02f, .028f), fr, [](float a) { return .1f * (1 - .2f * std::cos(2 * a)); }, 16,
         .006f, {.92f, .92f, .9f}, {.7f, .7f, .68f});
    box(fr.at(g.bodyC, .02f, 0, .03f), fr, .015f, .045f, .008f, {.08f, .08f, .08f}); // pickups
    box(fr.at(g.bodyC, -.08f, 0, .03f), fr, .015f, .045f, .008f, {.08f, .08f, .08f});
    box(fr.at(g.bodyC, -.15f, 0, .03f), fr, .012f, .05f, .01f, {.75f, .75f, .78f}); // bridge
    const float half = neckLen / 2;
    box(fr.at(g.bodyC, .12f + half, 0, .01f), fr, half, .028f, .016f, {.36f, .22f, .12f});
    box(fr.at(g.bodyC, .12f + half, 0, .028f), fr, half, .026f, .003f, {.1f, .07f, .06f}); // fretboard
    box(fr.at(g.bodyC, .2f + neckLen, .01f, .005f), fr, .08f, .04f, .012f, finish * .5f); // headstock
}

void shadow(V3 at, float rx, float rz) {
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    const V3 c = cam.view(at + V3{0, .01f, 0});
    if (c.z < .3f)
        return;
    v.push_back({cam.screen(c), rgb(0, 0, 0, 150), {0, 0}});
    for (int i = 0; i <= 16; ++i) {
        const float a = 2 * Pi * i / 16;
        const V3 p = cam.view(at + V3{std::cos(a) * rx, .01f, std::sin(a) * rz});
        v.push_back({cam.screen(p), rgb(0, 0, 0, 0), {0, 0}});
        if (i)
            idx.insert(idx.end(), {0, i, i + 1});
    }
    mesh(nullptr, v, idx);
}

// ------------------------------------------------------------ the band
const Style ninaS{{.93f, .78f, .68f}, {.12f, .12f, .14f}, {.22f, .42f, .28f}, {.16f, .16f, .2f}, {.9f, .9f, .9f},
                  {.96f, .94f, .86f}, 0, true};
const Style caioS{{.78f, .58f, .45f}, {.62f, .14f, .16f}, {.1f, .1f, .11f}, {.2f, .26f, .4f}, {.12f, .1f, .09f},
                  {.1f, .07f, .06f}, 1, false};
const Style lunaS{{.88f, .7f, .6f}, {.2f, .16f, .3f}, {.86f, .86f, .88f}, {.12f, .12f, .14f}, {.6f, .1f, .14f},
                  {.5f, .3f, .82f}, 3, true};
const Style bentoS{{.6f, .42f, .32f}, {.85f, .82f, .76f}, {.85f, .82f, .76f}, {.18f, .18f, .2f}, {.1f, .1f, .1f},
                   {.85f, .45f, .1f}, 2, false};

float bangOf(const Show &s, float amount) { return amount * std::pow(1 - s.ph, 3.0f); }

void singer(const Show &s) {
    const V3 root = {-2.35f, 0, .15f};
    const float nod = s.failed ? .45f : .05f + bangOf(s, .3f * s.energy);
    Rig k = rig(root, .3f, s.failed ? .2f : .05f, .04f * std::sin(float(s.beat) * Pi), s.failed ? .1f : .03f + .05f * s.energy * (1 - s.ph),
                nod, .17f);
    legs(k, ninaS);
    trunk(k, ninaS);
    head(k, ninaS, 0);
    // Mic stand in front of her.
    const V3 base = root + k.body.f * .38f;
    const V3 mic = k.headC + k.head.f * .17f + k.head.u * -.05f;
    cylinder(base, base + V3{0, .02f, 0}, .2f, .2f, {.15f, .15f, .17f}, 10);
    cylinder(base, {base.x, mic.y - .15f, base.z}, .012f, .012f, {.3f, .3f, .32f}, 5, false);
    cylinder({base.x, mic.y - .15f, base.z}, mic, .01f, .01f, {.3f, .3f, .32f}, 5, false);
    sphere(mic, .035f, {.5f, .5f, .52f}, 6, 4);
    arm(k.shoulderR, mic + V3{0, -.07f, 0}, k.body.r + V3{0, -1, 0}, ninaS);
    V3 hand;
    if (s.failed)
        hand = k.shoulderL + V3{-.05f, -.5f, .05f};
    else if (s.power || s.energy > .55f) {
        const float pump = s.power ? 1 : (s.ph < .5f ? 1 - s.ph * 2 : 0);
        hand = k.shoulderL + k.body.r * -.15f + V3{0, -.15f + .65f * pump, .15f};
    } else
        hand = k.shoulderL + V3{-.12f, -.42f + .04f * std::sin(float(s.beat) * Pi), .12f};
    arm(k.shoulderL, hand, k.body.r * -1 + V3{0, -1, 0}, ninaS);
}

void guitarist(const Show &in) {
    Show s = in;
    if (outcome < 0)
        s.failed = true, s.power = false; // beaten: head down, guitar hanging
    if (outcome > 0)
        s.power = true, s.failed = false, s.energy = 1; // the winner jumps with the guitar up
    float hop = 0;
    if (s.power) {
        const float h = frac(s.beat / 2);
        hop = h < .5f ? std::sin(Pi * h * 2) * .28f : 0;
    }
    const V3 root = {2.3f, hop, -.1f};
    const float nod = s.failed ? .5f : .06f + bangOf(s, .45f * s.energy) - .35f * s.miss;
    Rig k = rig(root, -.32f, s.failed ? .25f : .08f + bangOf(s, .12f * s.energy) - .2f * s.miss, 0,
                s.failed ? .12f : .04f + .07f * s.energy * (1 - s.ph) + (s.power ? .06f : 0), nod, s.power ? .26f : .2f);
    Style caio = caioS;
    caio.jacket = jacketColour;
    legs(k, caio);
    trunk(k, caio);
    head(k, caio, s.failed ? 0 : (1 - s.ph) * s.energy * .8f - s.miss);
    const Axe g = strap(k, s.failed ? -.1f : (s.power ? 1.05f : .5f + .06f * std::sin(float(s.beat) * Pi * .5f)));
    axe(g, .48f, guitarFinish, false);
    int lane = -1;
    for (int i = 0; i < 5; ++i)
        if (s.held & (1 << i)) {
            lane = i;
            break;
        }
    const float along = lane < 0 ? .32f : .5f - lane * .08f;
    const V3 fret = g.bodyC + g.along * (.12f + .48f * (1 - along)) + g.face * .04f - g.across * .03f;
    const V3 pick = g.bodyC + g.along * -.04f + g.face * .07f + g.across * (.04f - .12f * s.hit);
    arm(k.shoulderR, fret, k.body.r + V3{0, -1, -.3f}, caio);
    // A winner's fist goes up instead of strumming.
    const V3 strumHand = outcome > 0 ? k.shoulderL + V3{-.15f, .55f + .08f * std::sin(float(s.beat) * Pi), .1f} : pick;
    arm(k.shoulderL, strumHand, k.body.r * -1 + V3{0, -.4f, -.6f}, caio);
}

void bassist(const Show &s) {
    const V3 root = {-3.9f, 0, -2.7f};
    const float sway = std::sin(float(s.beat) * Pi * .5f);
    Rig k = rig(root, .45f, s.failed ? .2f : .04f + bangOf(s, .06f * s.energy), .05f * sway,
                s.failed ? .08f : .03f + .05f * s.energy * (1 - s.ph), s.failed ? .4f : .04f + bangOf(s, .22f * s.energy),
                .22f);
    legs(k, lunaS);
    trunk(k, lunaS);
    head(k, lunaS, 0);
    const Axe g = strap(k, s.failed ? -.1f : .35f + (s.power ? .3f : 0));
    axe(g, .66f, {.62f, .1f, .14f}, true);
    const float walk = frac(s.beat / 4);
    const V3 fret = g.bodyC + g.along * (.4f + .2f * std::sin(walk * 2 * Pi)) + g.face * .04f - g.across * .03f;
    const V3 pluck = g.bodyC + g.along * -.02f + g.face * .07f + g.across * (.02f - .06f * (1 - frac(s.beat * 2)));
    arm(k.shoulderR, fret, k.body.r + V3{0, -1, -.3f}, lunaS);
    arm(k.shoulderL, pluck, k.body.r * -1 + V3{0, -.4f, -.6f}, lunaS);
}

// The duel rivals: original characters, each with a look of their own.
void rivalFigure(const Show &in) {
    Show s = in;
    if (outcome > 0)
        s.failed = true; // beaten
    if (outcome < 0)
        s.failed = false, s.energy = 1;
    const float cheer = outcome < 0 ? std::sin(Pi * frac(s.beat / 2)) * .3f * (frac(s.beat / 2) < .5f ? 1.f : 0.f) : 0;
    // A step closer than the singer: this is their show too. Against a whole
    // band they come further downstage, clear of the bassist behind them.
    const V3 root = rivalSpot + V3{0, cheer, 0};
    const float bang = s.failed ? 0 : bangOf(s, .35f);
    const float lean = .06f + (rivalOn == 1 ? bangOf(s, .15f) : 0) - .25f * rivalHurt;
    Rig k = rig(root, .32f, lean, rivalOn == 0 ? .05f * std::sin(float(s.beat) * Pi * .5f) : 0,
                .04f + .05f * (1 - s.ph), (rivalOn == 2 ? .02f : .05f + bang) - .3f * rivalHurt, .2f);
    Style st;
    float neck = .55f;
    if (rivalOn == 0) { // Dona Rubi: silver bun, red dress, cat-eye glasses, a resonator
        st = {{.84f, .66f, .54f}, {.62f, .06f, .12f}, {.55f, .04f, .1f}, {.62f, .06f, .12f}, {.08f, .06f, .06f},
              {.85f, .85f, .88f}, 3, true};
        legs(k, st);
        // The dress: a flared skirt over the legs.
        frustum(k.pelvis + V3{0, -.5f, 0}, k.pelvis + k.body.u * .08f, k.body.f, .3f, .24f, .17f, .12f, st.top);
        trunk(k, st);
        const Frame &h = k.head;
        ellipsoid(k.headC, h, .1f, .125f, .11f, st.skin);
        ellipsoid(h.at(k.headC, 0, .035f, -.01f), h, .112f, .11f, .118f, st.hair, 10, 4, 0);
        sphere(h.at(k.headC, 0, .1f, -.1f), .07f, st.hair, 8, 5);                              // the bun
        box(h.at(k.headC, 0, .015f, .1f), h, .1f, .022f, .02f, {.75f, .1f, .2f});              // cat-eye frames
        box(h.at(k.headC, -.09f, .04f, .095f), h, .025f, .012f, .015f, {.75f, .1f, .2f});
        box(h.at(k.headC, .09f, .04f, .095f), h, .025f, .012f, .015f, {.75f, .1f, .2f});
        neck = .35f;
    } else if (rivalOn == 1) { // Viktor Kade: blond mane, leopard jacket, white double neck
        st = {{.9f, .72f, .6f}, {.08f, .08f, .09f}, {.9f, .62f, .16f}, {.06f, .06f, .07f}, {.5f, .08f, .08f},
              {.95f, .85f, .5f}, 1, true};
        legs(k, st);
        trunk(k, st);
        for (int i = 0; i < 9; ++i) { // leopard spots on the jacket
            const float a = (i % 3 - 1) * .1f, b = .12f + (i / 3) * .12f;
            const V3 at = k.pelvis + k.body.u * b + k.body.r * (a + (a > 0 ? .07f : -.07f)) + k.body.f * .135f;
            box(at, k.body, .018f, .014f, .004f, {.25f, .15f, .05f});
        }
        head(k, st, s.failed ? 0 : (1 - s.ph) * .9f);
        neck = .6f;
    } else if (rivalOn == 2) { // MK-77: chrome boxes, a visor and an antenna
        st = {{.7f, .72f, .78f}, {.55f, .58f, .64f}, {.4f, .42f, .48f}, {.3f, .32f, .36f}, {.2f, .2f, .22f},
              {.2f, .2f, .22f}, 0, true};
        legs(k, st);
        box(k.pelvis, k.body, .17f, .1f, .12f, st.pants);
        box(k.pelvis + k.body.u * .3f, k.body, .22f, .2f, .13f, st.top);
        box(k.pelvis + k.body.u * .3f + k.body.f * .135f, k.body, .08f, .06f, .005f, rivalColour); // chest light
        cylinder(k.neck - k.body.u * .04f, k.headC - k.head.u * .08f, .05f, .05f, st.jacket, 6);
        box(k.headC, k.head, .12f, .11f, .11f, st.top);
        emissive = true;
        box(k.head.at(k.headC, 0, .01f, .112f), k.head, .1f, .025f, .003f,
            rivalColour * (.7f + .3f * std::pow(1 - s.ph, 2.0f)));
        emissive = false;
        cylinder(k.head.at(k.headC, .06f, .11f, 0), k.head.at(k.headC, .08f, .28f, 0), .008f, .006f, st.jacket, 4, false);
        sphere(k.head.at(k.headC, .08f, .29f, 0), .02f, rivalColour, 6, 4);
        neck = .7f;
    } else if (rivalOn == 4) { // Capitã Maré: tricorn hat, eye patch, a navy coat with gold buttons
        const V3 navy = {.08f, .12f, .3f}, gold = {.9f, .7f, .2f};
        st = {{.78f, .58f, .44f}, {.92f, .9f, .85f}, navy, {.2f, .14f, .1f}, {.1f, .07f, .05f}, {.15f, .08f, .05f}, 1, true};
        legs(k, st);
        // The long coat tails first.
        frustum(k.pelvis + k.body.u * -.55f - k.body.f * .02f, k.pelvis + k.body.u * .1f, k.body.f, .26f, .14f, .2f, .12f, navy);
        trunk(k, st);
        for (int i = 0; i < 3; ++i)
            for (int side : {-1, 1})
                sphere(k.pelvis + k.body.u * (.16f + i * .1f) + k.body.r * (side * .09f) + k.body.f * .14f, .015f, gold, 5, 3);
        box(k.pelvis + k.body.u * .02f + k.body.f * .12f, k.body, .18f, .03f, .01f, {.1f, .06f, .04f}); // belt
        head(k, st, s.failed ? 0 : (1 - s.ph) * .5f);
        const Frame &h = k.head;
        const V3 c = k.headC;
        box(h.at(c, -.045f, .015f, .115f), h, .03f, .028f, .006f, {.03f, .03f, .03f});                   // eye patch
        cylinder(h.at(c, -.11f, .06f, 0), h.at(c, .11f, .06f, 0), .006f, .006f, {.03f, .03f, .03f}, 4, false); // its strap
        // The tricorn: a crown and three upturned flaps.
        cylinder(h.at(c, 0, .1f, -.01f), h.at(c, 0, .2f, -.01f), .1f, .085f, {.06f, .05f, .05f}, 10);
        for (int i = 0; i < 3; ++i) {
            const float a = i * 2 * Pi / 3 + Pi / 2;
            const V3 dir = h.r * std::cos(a) + h.f * std::sin(a);
            box(h.at(c, 0, .12f, -.01f) + dir * .12f, frameFrom(h.u + dir * -.6f, dir), .1f, .045f, .008f, {.06f, .05f, .05f});
        }
        box(h.at(c, 0, .16f, .11f), h, .03f, .02f, .004f, gold); // badge
        neck = .5f;
    } else if (rivalOn == 5) { // Grizzlo: a big white yeti with a blue face
        const V3 fur = {.9f, .92f, .95f}, face = {.35f, .5f, .7f};
        st = {face, fur, fur, fur, {.3f, .35f, .45f}, fur, 0, true};
        legs(k, st);
        ellipsoid(k.pelvis + k.body.u * .28f, k.body, .3f, .38f, .22f, fur, 12, 8); // the belly
        for (int i = 0; i < 14; ++i) { // shaggy tufts
            const float a = hash01(uint32_t(i * 7)) * 2 * Pi, y = .05f + .5f * hash01(uint32_t(i * 11));
            sphere(k.pelvis + k.body.u * y + k.body.r * (std::cos(a) * .27f) + k.body.f * (std::sin(a) * .18f), .07f, fur * .95f, 6, 4);
        }
        const Frame &h = k.head;
        const V3 c = k.headC + k.body.u * .04f;
        ellipsoid(c, h, .15f, .15f, .14f, fur, 10, 6);
        ellipsoid(h.at(c, 0, -.02f, .08f), h, .09f, .09f, .07f, face, 10, 5);
        for (int side : {-1, 1}) {
            box(h.at(c, side * .04f, .02f, .145f), h, .015f, .015f, .005f, {.05f, .05f, .08f});
            cylinder(h.at(c, side * .1f, .1f, 0), h.at(c, side * .16f, .2f, -.02f), .025f, .004f, {.85f, .82f, .7f}, 5, false); // little horns
            box(h.at(c, side * .025f, -.07f, .15f), h, .012f, .02f, .006f, {1, 1, 1});                                        // fangs
        }
        neck = .45f;
    } else if (rivalOn == 6) { // Zorg Nebula: a green, three-eyed alien in a silver suit
        const V3 green = {.35f, .8f, .3f}, silver = {.75f, .77f, .82f};
        st = {green, silver, silver, silver, {.3f, .3f, .35f}, green, 0, true};
        legs(k, st);
        trunk(k, st);
        box(k.pelvis + k.body.u * .32f + k.body.f * .13f, k.body, .06f, .06f, .005f, rivalColour); // the badge glows
        const Frame &h = k.head;
        const V3 c = k.headC + k.head.u * .04f;
        ellipsoid(c, h, .13f, .17f, .12f, green, 12, 7); // a tall bald head
        emissive = true;
        for (float x : {-.055f, 0.f, .055f})
            sphere(h.at(c, x, .02f + (x == 0 ? .04f : 0), .11f), .022f, V3{.1f, 1.f, .3f} * (.75f + .25f * std::pow(1 - s.ph, 2.0f)), 6, 4);
        emissive = false;
        for (int side : {-1, 1}) {
            const V3 base = h.at(c, side * .05f, .16f, 0), tip = base + h.dir(side * .06f, .14f, 0);
            cylinder(base, tip, .008f, .006f, green, 4, false);
            sphere(tip, .025f, rivalColour, 6, 4);
        }
        neck = .65f;
    } else { // Lived: an eighties album-cover devil, played for laughs
        const V3 red = {.8f, .1f, .08f}, black = {.05f, .04f, .05f}, ivory = {.93f, .88f, .74f};
        st = {red, red * .9f, black, black, {.12f, .05f, .05f}, black, 0, true};
        // The cape first, hanging behind him and swinging with the beat.
        const float swing = .06f * std::sin(float(s.beat) * Pi * .5f);
        frustum(k.pelvis + k.body.u * -.75f - k.body.f * .2f + k.body.r * swing, k.pelvis + k.body.u * .5f - k.body.f * .12f,
                k.body.f, .42f, .02f, .24f, .02f, {.35f, .02f, .04f});
        legs(k, st);
        trunk(k, st);
        // Studded shoulder pads, very much of their decade.
        for (int side : {-1, 1}) {
            const V3 pad = (side < 0 ? k.shoulderL : k.shoulderR) + k.body.u * .04f;
            ellipsoid(pad, k.body, .09f, .05f, .09f, black, 8, 4);
            for (int i = 0; i < 3; ++i)
                cylinder(pad + k.body.r * (side * (.02f + i * .03f)) + k.body.u * .03f,
                         pad + k.body.r * (side * (.04f + i * .03f)) + k.body.u * .12f, .018f, .002f, {.75f, .75f, .8f}, 4, false);
        }
        // The tail: it curls up behind him and ends in an arrowhead.
        V3 at = k.pelvis - k.body.f * .1f - k.body.u * .05f;
        for (int i = 0; i < 6; ++i) {
            const float a = float(i) / 5;
            const V3 next = at + k.body.f * -.09f + k.body.u * (-.06f + .13f * a) +
                            k.body.r * (.07f * std::sin(float(s.beat) * Pi + a * 3));
            cylinder(at, next, .028f - .015f * a, .028f - .015f * (a + .2f), red, 5, false);
            at = next;
        }
        frustum(at + k.body.u * .1f, at, k.body.r, .002f, .01f, .07f, .01f, red);
        const Frame &h = k.head;
        const V3 c = k.headC;
        ellipsoid(c, h, .115f, .14f, .12f, red); // a head a size too big: he is a cartoon
        ellipsoid(h.at(c, 0, .08f, -.05f), h, .14f, .1f, .12f, black, 10, 4); // teased black hair
        for (int side : {-1, 1}) {
            // Horns, curling up and out.
            const V3 base = h.at(c, side * .07f, .11f, .03f);
            const V3 mid = base + h.dir(side * .12f, .08f, 0), tip = mid + h.dir(side * .02f, .16f, .05f);
            cylinder(base, mid, .042f, .028f, ivory, 7, false);
            cylinder(mid, tip, .028f, .002f, ivory, 7, false);
            // Pointed ears.
            frustum(h.at(c, side * .1f, 0, 0), h.at(c, side * .16f, .06f, -.02f), h.f, .02f, .01f, .002f, .002f, red);
            // Wicked eyebrows.
            box(h.at(c, side * .042f, .045f, .1f), frameFrom(h.u + h.r * (side * .55f), h.f), .012f, .035f, .01f, black);
        }
        emissive = true; // glowing eyes and a grin full of teeth
        for (int side : {-1, 1})
            box(h.at(c, side * .04f, .015f, .105f), h, .022f, .011f, .004f,
                V3{1.f, .85f, .1f} * (.8f + .2f * std::pow(1 - s.ph, 2.0f)));
        emissive = false;
        box(h.at(c, 0, -.055f, .1f), h, .05f, .012f, .01f, {.95f, .95f, .92f});
        // A pointed goatee.
        frustum(h.at(c, 0, -.25f, .1f), h.at(c, 0, -.11f, .08f), h.f, .003f, .003f, .035f, .03f, black);
        neck = .6f;
    }
    // The guitar: a resonator, a double neck, or a V.
    const Axe g = strap(k, s.failed ? -.1f : neck + .08f * std::sin(float(s.beat) * Pi * .5f));
    Frame fr;
    fr.r = g.along, fr.u = g.across, fr.f = g.face;
    if (rivalOn == 0) {
        slab(fr.at(g.bodyC, -.04f, 0, 0), fr, [](float) { return .2f; }, 24, .07f, {.78f, .78f, .82f}, {.5f, .5f, .55f});
        slab(fr.at(g.bodyC, -.04f, 0, .036f), fr, [](float) { return .1f; }, 16, .005f, {.55f, .55f, .6f}, {.4f, .4f, .45f});
        box(fr.at(g.bodyC, .12f + .24f, 0, .01f), fr, .24f, .028f, .016f, {.3f, .18f, .1f});
    } else if (rivalOn == 1) {
        for (float off : {-.06f, .07f}) {
            box(fr.at(g.bodyC, .14f + .24f, off, .01f), fr, .24f, .024f, .014f, {.3f, .2f, .1f});
            box(fr.at(g.bodyC, .66f, off, .005f), fr, .06f, .035f, .012f, {.92f, .92f, .9f});
        }
        slab(fr.at(g.bodyC, -.04f, 0, 0), fr, [](float a) { return .2f * (1 + .25f * std::pow(std::max(0.0f, std::cos(a - 1.3f)), 6.0f)); },
             28, .05f, {.92f, .92f, .9f}, {.6f, .6f, .58f});
    } else if (rivalOn == 3) {
        // A guitar with a spiky body and a pitchfork for a headstock.
        slab(fr.at(g.bodyC, -.04f, 0, 0), fr, [](float a) { return .17f * (1 + .35f * std::pow(std::abs(std::cos(a * 2.5f)), 4.0f)); },
             30, .05f, {.12f, .02f, .02f}, {.7f, .1f, .06f});
        box(fr.at(g.bodyC, .12f + .24f, 0, .01f), fr, .24f, .024f, .016f, {.08f, .06f, .06f});
        const V3 steel = {.8f, .8f, .84f};
        box(fr.at(g.bodyC, .62f, 0, .01f), fr, .015f, .09f, .015f, steel);
        for (float off : {-.08f, 0.f, .08f})
            frustum(fr.at(g.bodyC, .63f, off, .01f), fr.at(g.bodyC, .76f, off, .01f), g.face, .012f, .012f, .002f, .002f, steel);
    } else if (rivalOn == 4) {
        // A ship's-wood body with a brass anchor on it.
        slab(fr.at(g.bodyC, -.04f, 0, 0), fr, [](float) { return .19f; }, 22, .06f, {.45f, .26f, .12f}, {.3f, .17f, .08f});
        box(fr.at(g.bodyC, .12f + .24f, 0, .01f), fr, .24f, .025f, .016f, {.3f, .18f, .1f});
        const V3 brass = {.85f, .65f, .25f};
        box(fr.at(g.bodyC, -.04f, 0, .035f), fr, .015f, .11f, .006f, brass);
        box(fr.at(g.bodyC, -.04f, -.09f, .035f), fr, .07f, .015f, .006f, brass);
        box(fr.at(g.bodyC, -.04f, .07f, .035f), fr, .04f, .012f, .006f, brass);
    } else if (rivalOn == 5) {
        // A flying V carved out of ice, glowing at the edges.
        for (int side : {-1, 1})
            box(fr.at(g.bodyC, -.12f, side * .1f, 0), Frame{frameFrom(g.across * float(side) + g.along * -.6f, g.face).u * -1,
                                                              frameFrom(g.across * float(side) + g.along * -.6f, g.face).r,
                                                              g.face},
                .06f, .22f, .03f, {.7f, .9f, 1.f});
        box(fr.at(g.bodyC, .12f + .24f, 0, .01f), fr, .24f, .026f, .016f, {.6f, .8f, .95f});
        emissive = true;
        box(fr.at(g.bodyC, .12f + .24f, 0, .028f), fr, .24f, .005f, .002f, rivalColour);
        emissive = false;
    } else if (rivalOn == 6) {
        // A flying saucer with a neck, its rim lights chasing round.
        slab(fr.at(g.bodyC, -.04f, 0, 0), fr, [](float) { return .2f; }, 24, .05f, {.7f, .72f, .78f}, {.45f, .47f, .52f});
        box(fr.at(g.bodyC, .12f + .24f, 0, .01f), fr, .24f, .024f, .016f, {.4f, .42f, .48f});
        emissive = true;
        for (int i = 0; i < 6; ++i) {
            const float a = i * Pi / 3 + float(s.beat) * Pi / 2;
            sphere(fr.at(g.bodyC, -.04f + .17f * std::cos(a), .17f * std::sin(a), .03f), .018f, rivalColour, 5, 3);
        }
        emissive = false;
    } else {
        // Two long wings swept back from the neck: a V.
        for (int side : {-1, 1})
            box(fr.at(g.bodyC, -.12f, side * .1f, 0), Frame{frameFrom(g.across * float(side) + g.along * -.6f, g.face).u * -1,
                                                              frameFrom(g.across * float(side) + g.along * -.6f, g.face).r,
                                                              g.face},
                .05f, .2f, .025f, {.1f, .1f, .12f});
        box(fr.at(g.bodyC, .12f + .24f, 0, .01f), fr, .24f, .026f, .016f, {.15f, .15f, .17f});
        emissive = true;
        box(fr.at(g.bodyC, .12f + .24f, 0, .028f), fr, .24f, .006f, .002f, rivalColour);
        emissive = false;
    }
    const float walk = frac(s.beat / 2);
    const V3 fret = g.bodyC + g.along * (.3f + .15f * std::sin(walk * 2 * Pi)) + g.face * .04f - g.across * .03f;
    V3 pick = g.bodyC + g.along * -.04f + g.face * .07f + g.across * (.03f - .1f * (1 - frac(s.beat * 2)));
    if (outcome < 0) // the rival gloats with a fist in the air
        pick = k.shoulderL + V3{-.15f, .55f + .08f * std::sin(float(s.beat) * Pi), .1f};
    if (outcome > 0) // and hangs their head when beaten
        pick = k.shoulderL + V3{-.05f, -.5f, .08f};
    arm(k.shoulderR, fret, k.body.r + V3{0, -1, -.3f}, st);
    arm(k.shoulderL, pick, k.body.r * -1 + V3{0, -.4f, -.6f}, st);
}

void drummer(const Show &s) {
    const V3 seat = {0, .5f, -4.15f};
    const Frame fr; // facing the audience
    const int beatNo = int(std::floor(s.beat));
    const float ph2 = frac(s.beat * 2);
    // Kit: kick at the front, toms above it, snare and hats to his left (screen right).
    const V3 shell = {.62f, .08f, .12f}, headCol = {.9f, .88f, .84f}, brass = {.86f, .66f, .28f};
    const V3 kick = seat + V3{0, .02f, .62f};
    cylinder(kick + V3{0, .28f, -.18f}, kick + V3{0, .28f, .2f}, .28f, .28f, shell, 14);
    cylinder(kick + V3{0, .28f, .2f}, kick + V3{0, .28f, .205f}, .26f, .26f, headCol, 14);
    const V3 snare = seat + V3{.3f, .62f, .38f}, tom1 = seat + V3{-.14f, .82f, .48f}, tom2 = seat + V3{.14f, .82f, .48f};
    const V3 floorTom = seat + V3{-.42f, .5f, .3f};
    for (V3 d : {snare, tom1, tom2, floorTom}) {
        cylinder(d - V3{0, .07f, 0}, d + V3{0, .07f, 0}, .14f, .14f, d.x == snare.x ? V3{.8f, .8f, .82f} : shell, 10);
        cylinder(d + V3{0, .07f, 0}, d + V3{0, .075f, 0}, .135f, .135f, headCol, 10, true);
    }
    const float crashWob = s.failed ? 0 : ease(std::fmod(s.beat, 4.0), .9) * s.energy;
    const V3 hat = seat + V3{.55f, .78f, .3f}, crash = seat + V3{.5f, 1.25f, .2f}, ride = seat + V3{-.6f, 1.05f, .25f};
    for (V3 c : {hat, crash, ride}) {
        cylinder(c - V3{0, .9f, 0}, c, .012f, .012f, {.3f, .3f, .32f}, 4, false);
        const float tilt = c.y == crash.y ? .08f + .1f * crashWob : .04f;
        const Frame cf = frameFrom({0, 1, tilt}, {0, 0, 1});
        ellipsoid(c, cf, c.y == crash.y ? .26f : .2f, .012f, c.y == crash.y ? .26f : .2f, brass, 12, 2);
    }
    // Drummer on his stool.
    cylinder(seat - V3{0, .45f, 0}, seat - V3{0, .02f, 0}, .04f, .04f, {.2f, .2f, .22f}, 5);
    cylinder(seat - V3{0, .03f, 0}, seat, .17f, .17f, {.1f, .1f, .1f}, 10);
    Rig k = rig(seat + V3{0, -.95f + .05f, 0}, 0, .08f + bangOf(s, .1f * s.energy), 0, 0,
                s.failed ? .4f : .06f + bangOf(s, .35f * s.energy), .2f);
    k.footL = seat + V3{-.25f, -.5f, .45f}, k.footR = seat + V3{.25f, -.5f, .45f};
    k.hipL = seat + V3{-.1f, .05f, 0}, k.hipR = seat + V3{.1f, .05f, 0};
    for (int side : {-1, 1}) {
        const V3 hip = side < 0 ? k.hipL : k.hipR, foot = side < 0 ? k.footL : k.footR;
        const V3 knee = hip + V3{side * .06f, .05f, .42f};
        cylinder(hip, knee, .09f, .075f, bentoS.pants);
        cylinder(knee, foot + V3{0, .08f, 0}, .072f, .058f, bentoS.pants);
        box(foot + V3{0, .05f, .04f}, .06f, .05f, .11f, bentoS.shoes);
    }
    trunk(k, bentoS);
    head(k, bentoS, 0);
    const float lift = s.failed ? .1f : std::sin(Pi * ph2) * (.1f + .14f * s.energy);
    const bool backbeat = beatNo % 2 == 1 && ph2 < .5f;
    const bool crashHit = beatNo % 4 == 0 && frac(s.beat) < .5f && s.energy > .45f;
    const V3 rightT = (crashHit ? crash : hat) + V3{-.1f, .06f + lift, 0};
    const V3 leftT = snare + V3{-.06f, .1f + (backbeat ? lift : lift * .3f), -.05f};
    // His right hand rides the hats crossed over; screen left shoulder is his right.
    for (auto [sh, hand] : {std::pair{k.shoulderL, rightT}, std::pair{k.shoulderR, leftT}}) {
        const V3 elbow = ik(sh, hand, .29f, .27f, V3{sh.x < seat.x ? -1.f : 1.f, -1, -.2f});
        cylinder(sh, elbow, .062f, .052f, bentoS.skin);
        cylinder(elbow, hand, .048f, .04f, bentoS.skin);
        sphere(hand, .045f, bentoS.skin, 6, 4);
        const V3 stick = norm(hand - elbow);
        cylinder(hand, hand + stick * .32f, .01f, .008f, {.85f, .75f, .55f}, 4, false);
    }
    (void)fr;
}

// ------------------------------------------------------------ the set
std::array<V3, 9> lampPos;
int lampCount = 5;
void stageSet(const Show &s, SDL_Color key) {
    const V3 deck = {.13f, .12f, .14f}, metal = {.32f, .33f, .36f}, black = {.06f, .06f, .07f};
    // Floor, in planks so the light pools have something to fall across.
    for (int i = 0; i < 12; ++i) {
        const float x0 = -10 + i * (20.f / 12), x1 = x0 + 20.f / 12;
        flatQuad({x0, 0, 1.3f}, {x1, 0, 1.3f}, {x1, 0, -6}, {x0, 0, -6}, {0, 1, 0}, i % 2 ? deck : deck * .85f);
    }
    flatQuad({-10, -1.5f, 1.3f}, {10, -1.5f, 1.3f}, {10, 0, 1.3f}, {-10, 0, 1.3f}, {0, 0, 1}, black);
    // Back wall and the LED screen on it (emissive).
    if (vl->backdrop == 2) {
        // No back wall outdoors: a sunset sky behind a scaffold, drawn as is.
        emissive = true;
        const V3 sky[4] = {{.12f, .06f, .2f}, {.45f, .14f, .3f}, {.95f, .45f, .2f}, {1.f, .7f, .35f}};
        for (int i = 0; i < 3; ++i) {
            const float y0 = 9 - i * 3.f, y1 = y0 - 3;
            tri({-30, y0, -14}, {30, y0, -14}, {30, y1, -14}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}, sky[i] * .5f + sky[i + 1] * .5f);
            tri({-30, y0, -14}, {30, y1, -14}, {-30, y1, -14}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}, sky[i] * .5f + sky[i + 1] * .5f);
        }
        emissive = false;
        for (float x : {-6.f, 6.f})
            box({x, 3.5f, -6}, .12f, 3.5f, .12f, {.2f, .2f, .22f});
        box({0, 7, -6}, 6.2f, .12f, .12f, {.2f, .2f, .22f});
    } else if (vl->backdrop == 0) {
        // Brick: rows of slightly different reds.
        for (int row = 0; row < 16; ++row)
            for (int col = 0; col < 12; ++col) {
                const float x0 = -12 + col * 2.f + (row % 2) * 1.f, y0 = row * .5f;
                const float k = .7f + .3f * hash01(uint32_t(row * 31 + col));
                flatQuad({x0 + .03f, y0 + .03f, -6}, {x0 + 1.97f, y0 + .03f, -6}, {x0 + 1.97f, y0 + .47f, -6},
                         {x0 + .03f, y0 + .47f, -6}, {0, 0, 1}, V3{.32f, .13f, .1f} * k);
            }
        flatQuad({-12, 0, -6.01f}, {12, 0, -6.01f}, {12, 8, -6.01f}, {-12, 8, -6.01f}, {0, 0, 1}, {.08f, .06f, .05f});
    } else if (vl->backdrop == 4) {
        // The glacier: a night sky, an aurora rippling across it, and walls of ice.
        emissive = true;
        flatQuad({-30, 0, -14}, {30, 0, -14}, {30, 12, -14}, {-30, 12, -14}, {0, 0, 1}, {.02f, .04f, .1f});
        for (int band = 0; band < 3; ++band)
            for (int i = 0; i < 24; ++i) {
                const float x0 = -24 + i * 2.f, x1 = x0 + 2;
                const float w0 = std::sin(x0 * .35f + float(s.time) * .6f + band * 2.1f), w1 = std::sin(x1 * .35f + float(s.time) * .6f + band * 2.1f);
                const float y = 6.5f + band * .9f;
                const V3 c = (band == 1 ? V3{.3f, .9f, .9f} : V3{.25f, 1.f, .45f}) * (.35f + .2f * band);
                flatQuad({x0, y + w0, -13.9f}, {x1, y + w1, -13.9f}, {x1, y + w1 + 1.4f, -13.9f}, {x0, y + w0 + 1.4f, -13.9f}, {0, 0, 1}, c);
            }
        emissive = false;
        for (int i = 0; i < 9; ++i) {
            const float x = -11 + i * 2.75f, h = 2.5f + 3.f * hash01(uint32_t(i * 17));
            box({x, h / 2, -6.5f}, 1.2f + .4f * hash01(uint32_t(i)), h / 2, .6f, V3{.55f, .75f, .9f} * (.8f + .2f * hash01(uint32_t(i * 3))));
        }
    } else if (vl->backdrop == 5) {
        // Orbit: black space full of stars, and a ringed planet hanging behind the stage.
        emissive = true;
        flatQuad({-30, -2, -14}, {30, -2, -14}, {30, 14, -14}, {-30, 14, -14}, {0, 0, 1}, {.01f, .01f, .03f});
        for (int i = 0; i < 120; ++i) {
            const float x = -26 + 52 * hash01(uint32_t(i * 7 + 1)), y = 13 * hash01(uint32_t(i * 13 + 5));
            const float r = .03f + .05f * hash01(uint32_t(i * 3)), tw = .6f + .4f * std::sin(float(s.time) * 3 + i);
            flatQuad({x - r, y - r, -13.9f}, {x + r, y - r, -13.9f}, {x + r, y + r, -13.9f}, {x - r, y + r, -13.9f}, {0, 0, 1},
                     V3{1, 1, 1} * tw);
        }
        emissive = false;
        sphere({7, 8.5f, -12.5f}, 3.2f, {.55f, .3f, .7f}, 18, 10);
        for (int i = 0; i < 32; ++i) { // the ring, edge-on and tilted
            const float a0 = i * 2 * Pi / 32, a1 = (i + 1) * 2 * Pi / 32;
            const V3 p0 = {7 + 5.2f * std::cos(a0), 8.5f + 1.1f * std::sin(a0) - .6f * std::cos(a0), -12.4f},
                     p1 = {7 + 5.2f * std::cos(a1), 8.5f + 1.1f * std::sin(a1) - .6f * std::cos(a1), -12.4f};
            emissive = true;
            flatQuad(p0, p1, p1 + V3{0, .18f, 0}, p0 + V3{0, .18f, 0}, {0, 0, 1}, {.75f, .65f, .5f});
            emissive = false;
        }
    } else if (vl->backdrop == 3) {
        // The Inferno: black rock split by glowing cracks of lava that breathe with the beat.
        flatQuad({-12, 0, -6.01f}, {12, 0, -6.01f}, {12, 8, -6.01f}, {-12, 8, -6.01f}, {0, 0, 1}, {.07f, .03f, .03f});
        emissive = true;
        const float breathe = .55f + .45f * std::pow(1 - s.ph, 2.0f);
        for (int c = 0; c < 14; ++c) {
            float x = -11.5f + c * 1.75f + hash01(uint32_t(c * 13)) * 1.0f, y = 0;
            while (y < 8) {
                const float nx = x + (hash01(uint32_t(c * 97 + int(y * 10))) - .5f) * 1.1f, ny = y + .5f + hash01(uint32_t(c * 31 + int(y * 7))) * .6f;
                const float w = .06f + .05f * hash01(uint32_t(c * 5 + int(y)));
                const V3 lava = V3{1.f, .35f + .3f * hash01(uint32_t(c + int(y * 3))), .05f} * breathe;
                flatQuad({x - w, y, -5.99f}, {x + w, y, -5.99f}, {nx + w, ny, -5.99f}, {nx - w, ny, -5.99f}, {0, 0, 1}, lava);
                x = nx, y = ny;
            }
        }
        // A pool of glow along the foot of the wall.
        flatQuad({-12, 0, -5.98f}, {12, 0, -5.98f}, {12, .35f, -5.98f}, {-12, .35f, -5.98f}, {0, 0, 1}, V3{.9f, .25f, .05f} * breathe);
        emissive = false;
    } else
        flatQuad({-12, 0, -6}, {12, 0, -6}, {12, 8, -6}, {-12, 8, -6}, {0, 0, 1}, {.04f, .04f, .05f});
    emissive = true;
    const int cols = vl->led == 1 ? 12 : 26, rows = vl->led == 1 ? 6 : 10;
    const float wallW = vl->led == 1 ? 4.2f : 10.4f, wallH = vl->led == 1 ? 2.0f : 3.6f;
    const float x0 = -wallW / 2, y0 = vl->led == 1 ? 1.6f : .8f, cw = wallW / cols, ch = wallH / rows;
    const V3 on = col3(key);
    for (int i = 0; i < (vl->led ? cols : 0); ++i) {
        const float wave = .5f + .5f * std::sin(float(s.beat) * Pi * .5f + i * .5f);
        const float level = s.failed ? 0 : (.12f + .8f * s.energy * wave) * (.7f + .3f * (1 - s.ph));
        const int litRows = int(std::round(level * rows));
        for (int r = 0; r < rows; ++r) {
            const bool lit = r < litRows;
            const float glowK = lit ? (r == litRows - 1 ? 1.f : .45f + .35f * s.energy) : .06f;
            const V3 c = lit ? on * glowK + V3{.15f, .15f, .15f} * (r == litRows - 1 ? 1.f : 0.f) : V3{.05f, .05f, .07f};
            const float x = x0 + i * cw, y = y0 + r * ch;
            flatQuad({x + .02f, y + .02f, -5.95f}, {x + cw - .02f, y + .02f, -5.95f}, {x + cw - .02f, y + ch - .02f, -5.95f},
                     {x + .02f, y + ch - .02f, -5.95f}, {0, 0, 1}, c);
        }
    }
    // Side screens at the arena and stadium: a pulse of the key colour.
    if (vl->led == 3)
        for (float x : {-7.4f, 7.4f}) {
            const float k = s.failed ? .05f : .25f + .5f * s.energy * std::pow(1 - s.ph, 2.0f);
            flatQuad({x - 1.2f, 2.2f, -1.6f}, {x + 1.2f, 2.2f, -1.6f}, {x + 1.2f, 4.0f, -1.6f}, {x - 1.2f, 4.0f, -1.6f},
                     {0, 0, 1}, on * k + V3{.05f, .05f, .07f});
        }
    emissive = false;
    // Drum riser.
    box({0, .25f, -4.2f}, 1.7f, .25f, 1.1f, {.1f, .1f, .12f});
    // Amps behind the strings, speaker stacks at the wings.
    for (V3 a : {V3{3.4f, .65f, -2.2f}, V3{-5.0f, .65f, -3.8f}}) {
        box(a, .55f, .65f, .3f, black);
        box(a + V3{0, -.05f, .31f}, .48f, .5f, .01f, {.12f, .12f, .13f});
        box(a + V3{0, .72f, 0}, .55f, .1f, .3f, {.18f, .16f, .14f});
    }
    for (float x : {-8.2f, 8.2f})
        for (int i = 0; i < 3; ++i) {
            box({x, .55f + i * 1.1f, -1.4f}, .75f, .55f, .55f, black);
            for (float dy : {-.22f, .22f})
                cylinder({x, .55f + i * 1.1f + dy, -.84f}, {x, .55f + i * 1.1f + dy, -.83f}, .2f, .2f, {.03f, .03f, .03f}, 10);
        }
    // Truss and lamps, or bulbs on a wire in the garage.
    lampCount = vl->lamps ? vl->lamps : 3;
    if (vl->lamps) {
        box({0, 5.3f, -1.2f}, 8.6f, .12f, .12f, metal);
        box({0, 5.62f, -1.2f}, 8.6f, .06f, .06f, metal);
        for (float x : {-8.6f, 8.6f})
            box({x, 2.7f, -1.2f}, .14f, 2.7f, .14f, metal);
    }
    for (int i = 0; i < lampCount; ++i) {
        const float x = lampCount == 1 ? 0 : -5.4f + 10.8f * i / float(lampCount - 1);
        if (vl->lamps) {
            lampPos[size_t(i)] = {x, 5.0f, -1.1f};
            cylinder({x, 5.2f, -1.2f}, {x, 4.95f, -1.0f}, .16f, .18f, {.08f, .08f, .09f}, 8);
        } else {
            const float bx = (i - 1) * 3.2f;
            lampPos[size_t(i)] = {bx, 3.3f, -.8f};
            cylinder({bx, 6, -.8f}, {bx, 3.4f, -.8f}, .01f, .01f, {.1f, .1f, .1f}, 4, false);
        }
    }
}

void beams(const Show &s, const SDL_Color key, float level) {
    for (int i = 0; i < lampCount; ++i) {
        const V3 lamp = lampPos[size_t(i)];
        const float swing = (s.failed ? 0 : .9f * s.energy) * std::sin(float(s.beat) * Pi * .5f + (i % 2) * Pi);
        const V3 spot = {lamp.x + swing * 1.6f - (lamp.x / 5.4f) * .6f, 0, -.6f + .6f * std::cos(float(s.beat) * Pi * .25f + i)};
        const V3 a = cam.view(lamp), b = cam.view(spot);
        if (a.z < .3f || b.z < .3f)
            continue;
        const SDL_FPoint pa = cam.screen(a), pb = cam.screen(b);
        const float wa = 6, wb = cam.focal * 1.1f / b.z;
        const float dx = pb.x - pa.x, dy = pb.y - pa.y, l = std::max(1.0f, std::hypot(dx, dy));
        const float nx = -dy / l, ny = dx / l;
        const float strobe = s.power ? (frac(s.beat * 2) < .5f ? 1.f : .5f) : 1.f;
        const SDL_Color c = alpha(key, (.1f + .12f * s.energy) * strobe * level);
        quad({pa.x + nx * wa, pa.y + ny * wa}, {pa.x - nx * wa, pa.y - ny * wa}, {pb.x - nx * wb, pb.y - ny * wb},
             {pb.x + nx * wb, pb.y + ny * wb}, c, c, alpha(c, .15f), alpha(c, .15f));
        glow(pb.x, pb.y, wb * 2.6f, wb * .8f, alpha(key, .3f * level * strobe));
        glow(pa.x, pa.y, 40, 40, alpha(mix(key, rgb(255, 255, 255), .6f), .95f * level));
    }
}

void sparks(const Show &s) {
    const double age = s.time - mem.powerStart;
    if (age < 0 || age > 1.8)
        return;
    for (float bx : {-2.4f, 2.6f}) {
        const V3 base = cam.view({bx, 0, 1.1f});
        if (base.z < .3f)
            continue;
        const SDL_FPoint p = cam.screen(base);
        for (uint32_t i = 0; i < 40; ++i) {
            const float t = float(age) - hash01(i * 3 + uint32_t(bx * 10 + 50)) * .5f;
            if (t < 0)
                continue;
            const float vx = (hash01(i * 7) - .5f) * 160, vy = -420 - hash01(i * 11) * 280;
            const float x = p.x + vx * t, y = p.y + vy * t + 560 * t * t, life = 1 - t / 1.3f;
            if (life <= 0 || y > p.y + 4)
                continue;
            glow(x, y, 10 + 10 * life, 10 + 10 * life, rgb(255, 210, 120, int(230 * life)));
        }
    }
}

void fireColumns(const Show &s) {
    // The Inferno burns all the time; anywhere else the columns are bought, and fire in star power.
    const bool inferno = vl->backdrop == 3;
    if (!inferno && (!pyroOn || !s.power))
        return;
    const float burst = (inferno && !s.power ? .45f : 1.f) * (.35f + .65f * std::pow(1 - s.ph, 1.5f));
    // Flanking the drum riser and behind the front line, where the highway leaves them in view.
    for (V3 at : {V3{-2.1f, .5f, -4.3f}, V3{2.1f, .5f, -4.3f}, V3{-3.6f, 0, -1.8f}, V3{3.5f, 0, -1.8f}}) {
        const float x = at.x;
        const V3 base = cam.view(at);
        if (base.z < .3f)
            continue;
        const SDL_FPoint p = cam.screen(base);
        const float h = cam.focal * (1.6f + 2.2f * burst) / base.z, w = cam.focal * .9f / base.z;
        for (int i = 0; i < 9; ++i) {
            const float t = i / 8.f;
            glow(p.x + std::sin(float(s.time) * 13 + i * 2 + x) * w * .15f, p.y - h * t, w * (1.4f - t),
                 w * (1.6f - t), rgb(255, int(210 - 130 * t), int(90 - 80 * t), int(255 * burst * (1 - t * .5f))));
        }
    }
}

void crowd(const Show &s, SDL_Color key, float level) {
    for (int row = 0; row < 2; ++row) {
        const int count = std::max(2, int((row ? 15 : 19) * (.3f + .7f * vl->crowd)));
        const float baseY = row ? 708.f : 652.f, r = row ? 24.f : 17.f;
        for (int i = 0; i < count; ++i) {
            const uint32_t id = uint32_t(row * 100 + i);
            const float x = (i + .5f) * W / count + (hash01(id * 3) - .5f) * 40;
            const float bob = std::pow(1 - frac(s.beat + hash01(id * 5) * .2), 3.0f) * 7 * s.energy;
            const float y = baseY + (hash01(id * 7) - .5f) * 14 + bob;
            const SDL_Color body = row ? rgb(12, 11, 16) : rgb(20, 18, 25);
            if (!s.failed && hash01(id * 11) < (s.energy * 1.1f - .1f) * (.5f + .5f * vl->crowd)) {
                const float wave = std::sin(float(s.beat) * Pi + hash01(id * 13) * 6) * .25f;
                for (int side : {-1, 1}) {
                    if (side > 0 && hash01(id * 17) < .5f)
                        continue;
                    const float a = Pi + side * (.35f + wave);
                    const float hx = x + side * r * .8f + std::sin(a) * r * 3.2f, hy = y + r * .6f + std::cos(a) * r * 3.2f;
                    thickLine(x + side * r * .8f, y + r * .6f, hx, hy, r * .45f, body);
                    disc(hx, hy, r * .3f, r * .3f, body, body, 8);
                }
            }
            disc(x, y + r * 1.6f, r * 1.7f, r * 1.1f, body, body, 18);
            disc(x, y, r * .8f, r, body, body, 16);
            disc(x, y - r * .45f, r * .75f, r * .45f, alpha(key, .5f * level), alpha(key, 0), 12);
        }
    }
}

// Camera: cuts between shots every eight bars, a slow drift inside each, and
// a push in while star power burns.
void placeCamera(const Show &s) {
    // The high view over the crowd is the main shot; the others are variations
    // of it, with the low hero angle kept rare so it still lands.
    const double shotLen = 32; // beats
    const float pick = hash01(uint32_t(std::floor(s.beat / shotLen)) * 7 + 3);
    int shot = pick < .3f ? 0 : pick < .55f ? 1 : pick < .8f ? 2 : pick < .92f ? 3 : 4;
    if (s.failed || s.beat < shotLen)
        shot = 0; // every song opens on the main shot
    const float u = frac(s.beat / shotLen);
    V3 from, to;
    switch (shot) {
    case 1: // high, drifting from the left
        from = {-1.8f + u * 1.2f, 3.1f, 6.5f}, to = {.2f, 1.0f, -1.4f};
        break;
    case 2: // high, drifting from the right
        from = {1.8f - u * 1.2f, 3.1f, 6.5f}, to = {-.2f, 1.0f, -1.4f};
        break;
    case 3: // steeper, looking down on the band
        from = {.4f - u * .8f, 3.7f, 6.2f}, to = {0, .9f, -1.4f};
        break;
    case 4: // low and heroic, now and then
        from = {-1.0f + u * 1.2f, .8f, 5.3f}, to = {0, 1.95f, -1.2f};
        break;
    default: // high, over the crowd: the main shot
        from = {1.2f - u * 1.4f, 3.0f, 6.7f}, to = {0, 1.0f, -1.4f};
    }
    const float push = s.power ? smooth(float(s.time - mem.powerStart) / 1.5f) : 0;
    from = from + (to - from) * (.14f * push);
    const float shake = ease(s.time - mem.powerStart, .5) * .06f;
    from = from + V3{std::sin(float(s.time) * 61) * shake, std::sin(float(s.time) * 47) * shake, 0};
    cam.look(from, to);
}
} // namespace

void reset() { mem = Memory{}; }

void draw(const Cues &input) {
    Cues c = input;
#ifndef __SWITCH__
    if (const char *force = std::getenv("SWITCH_HERO_STAGE")) { // power | danger | failed | hot
        const std::string f = force;
        if (f == "power")
            c.power = true;
        else if (f == "danger")
            c.meter = .15;
        else if (f == "failed")
            c.failed = true;
        else if (f == "hot")
            c.multiplier = 4, c.meter = .95;
        if (const char *b = std::getenv("SWITCH_HERO_STAGE_BEAT"))
            c.beat += std::atof(b);
        if (const char *v = std::getenv("SWITCH_HERO_VENUE"))
            c.venue = std::atoi(v);
        if (const char *r = std::getenv("SWITCH_HERO_RIVAL")) {
            static const unsigned colors[7] = {0xd2283c, 0xffb428, 0x3cdcff, 0xff3a14, 0x1ec8aa, 0x9adcff, 0x8cff3c};
            c.rival = std::atoi(r) % 7, c.rivalColor = colors[c.rival];
            c.bandDuel = std::getenv("SWITCH_HERO_BAND_DUEL") != nullptr;
        }
        if (const char *o = std::getenv("SWITCH_HERO_OUTCOME"))
            c.outcome = std::atoi(o);
        if (std::getenv("SWITCH_HERO_PYRO"))
            c.pyro = true, c.guitarColor = 0xd9a93a, c.jacketColor = 0xdcdcde;
    }
#endif
    if (c.time < mem.lastTime - .25 || c.hits < mem.hits || c.misses < mem.misses)
        reset();
    if (c.hits > mem.hits)
        mem.lastHit = c.time;
    if (c.misses > mem.misses)
        mem.lastMiss = c.time;
    if (c.power && !mem.power)
        mem.powerStart = c.time;
    mem.hits = c.hits, mem.misses = c.misses, mem.power = c.power, mem.lastTime = c.time;

    Show s;
    s.time = c.time, s.beat = c.beat, s.ph = frac(c.beat), s.power = c.power, s.failed = c.failed, s.held = c.held;
    s.danger = !c.failed && c.meter < c.meterRed;
    s.energy = clamp01(.2f + .17f * float(c.multiplier - 1) + .45f * float(c.meter - .4));
    if (c.power)
        s.energy = 1;
    if (c.failed)
        s.energy = 0;
    s.hit = ease(c.time - mem.lastHit, .14), s.miss = ease(c.time - mem.lastMiss, .5);

    vl = &venueLooks[c.venue >= 0 && c.venue < 12 ? c.venue : 12];
    guitarFinish = hexColor(c.guitarColor), jacketColour = hexColor(c.jacketColor), pyroOn = c.pyro;
    rivalOn = c.rival >= 0 && c.rival <= 6 ? c.rival : -1;
    rivalColour = hexColor(c.rivalColor);
    rivalHurt = ease(c.time - c.rivalHurtAt, .6), rivalStrike = ease(c.time - c.rivalStrikeAt, .8);
    outcome = rivalOn >= 0 ? c.outcome : 0;
    SDL_Color key, fill;
    float level = vl->level;
    if (c.power)
        key = rgb(120, 205, 255), fill = rgb(60, 110, 220);
    else if (s.danger)
        key = rgb(255, 70, 55), fill = rgb(140, 30, 30), level *= .75f;
    else if (c.failed)
        key = rgb(110, 110, 130), fill = rgb(40, 40, 60), level *= .35f;
    else {
        const float t = .5f + .5f * std::sin(float(c.beat) * Pi / 16);
        key = mix(vl->keyA, vl->keyB, t), fill = mix(vl->fillA, vl->fillB, t);
    }
    placeCamera(s);
    const float pulse = .85f + .15f * std::pow(1 - s.ph, 2.0f) * s.energy;
    lit.keyDir = norm({-.45f, .8f, .55f});
    lit.fillDir = norm({.6f, .25f, .7f});
    lit.key = col3(key) * (1.15f * level * pulse);
    lit.fill = col3(fill) * (.45f * level);
    lit.rim = col3(mix(key, rgb(255, 255, 255), .35f)) * (.55f * level);
    lit.ambient = {.1f, .1f, .13f};

    rect(0, 0, W, H, rgb(5, 5, 8));
    stageSet(s, key);
    flushTris();
    // Light pools and shadows on the floor, under everyone.
    const bool wholeBand = rivalOn < 0 || c.bandDuel;
    rivalSpot = c.bandDuel ? V3{-1.35f, 0, .85f} : V3{-2.05f, 0, .5f};
    const std::vector<V3> standing = !wholeBand ? std::vector<V3>{{-2.05f, 0, .5f}, {2.3f, 0, -.1f}}
                                   : rivalOn >= 0 ? std::vector<V3>{rivalSpot, {2.3f, 0, -.1f}, {-3.9f, 0, -2.7f}}
                                                  : std::vector<V3>{{-2.35f, 0, .15f}, {2.3f, 0, -.1f}, {-3.9f, 0, -2.7f}};
    for (V3 p : standing) {
        const V3 v = cam.view(p);
        if (v.z > .3f) {
            const SDL_FPoint q = cam.screen(v);
            glow(q.x, q.y, cam.focal * 3.2f / v.z, cam.focal * .9f / v.z, alpha(key, .35f * level));
        }
        shadow(p, .45f, .3f);
    }
    if (rivalOn >= 0 && !c.bandDuel) {
        // A duel is one on one: the band clears the stage for the guitarist and the rival.
        rivalFigure(s);
        guitarist(s);
    } else if (rivalOn >= 0) {
        // The band against the rival: the rival takes the singer's spot.
        drummer(s);
        bassist(s);
        rivalFigure(s);
        guitarist(s);
    } else {
        drummer(s);
        bassist(s);
        singer(s);
        guitarist(s);
    }
    flushTris();
    beams(s, key, level);
    if (s.miss > 0) {
        const V3 v = cam.view({2.3f, 1.7f, -.1f});
        if (v.z > .3f) {
            const SDL_FPoint q = cam.screen(v);
            glow(q.x, q.y, 220, 220, rgb(255, 40, 40, int(110 * s.miss)));
        }
    }
    if (rivalOn >= 0) {
        // The rival's own light on their side, and a flash of it when they strike.
        const V3 v = cam.view(rivalSpot + V3{0, 1.2f, 0});
        if (v.z > .3f) {
            const SDL_FPoint q = cam.screen(v);
            const SDL_Color rc = rgb(int(rivalColour.x * 255), int(rivalColour.y * 255), int(rivalColour.z * 255));
            glow(q.x, q.y, cam.focal * 2.6f / v.z, cam.focal * 3.2f / v.z, alpha(rc, .22f + .5f * rivalStrike));
            if (rivalHurt > 0)
                glow(q.x, q.y - 40, 240, 240, rgb(255, 255, 255, int(150 * rivalHurt)));
        }
        if (rivalStrike > 0)
            rect(0, 0, W, H, rgb(int(rivalColour.x * 255), int(rivalColour.y * 255), int(rivalColour.z * 255), int(60 * rivalStrike)));
    }
    sparks(s);
    fireColumns(s);
    crowd(s, key, level);
}
} // namespace fret::stage
