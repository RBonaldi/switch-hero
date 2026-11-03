// The stage behind the highway. See stage.hpp.
//
// The band is original to this game: Nina on vocals (short platinum hair,
// green bomber), Caio on guitar (the player: red flannel, long dark hair, a
// teal offset-body guitar) and Bento on drums (beanie, striped tank top).
// They are paper puppets: limbs are thick lines between joints placed by
// two-bone IK, so a hand goes where it is needed (pick, neck, drum, mic) and
// the elbow follows.
#include "stage.hpp"
#include "look.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>
#include <cmath>
#include <tuple>
#include <utility>

namespace fret::stage {
using namespace look;
namespace {
using P = SDL_FPoint;
constexpr float Pi = 3.14159265f;

struct Memory {
    int hits = 0, misses = 0;
    double lastHit = -1e9, lastMiss = -1e9, powerStart = -1e9, lastTime = -1e9;
    bool power = false;
} mem;

float frac(double v) { return float(v - std::floor(v)); }
float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
float ease(double age, double life) { // 1 at the event, 0 after `life`
    if (age < 0 || age > life)
        return 0;
    const float t = float(1 - age / life);
    return t * t;
}
P add(P a, P b) { return {a.x + b.x, a.y + b.y}; }
P sub(P a, P b) { return {a.x - b.x, a.y - b.y}; }
P scale(P a, float s) { return {a.x * s, a.y * s}; }
P lerp(P a, P b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }
float len(P a) { return std::hypot(a.x, a.y); }
// Unit vector at `a` radians from straight down, turning toward screen right.
P dir(float a) { return {std::sin(a), std::cos(a)}; }
SDL_Color rgb(int r, int g, int b, int a = 255) { return {Uint8(r), Uint8(g), Uint8(b), Uint8(a)}; }
SDL_Color shade(SDL_Color c, float k) { return mix(c, rgb(6, 6, 10, c.a), k); }

// Two-bone IK: the joint between `root` and `target`, bent to the side `bend`
// (+1 bends to the right of the root->target line, -1 to the left).
P joint(P root, P target, float l1, float l2, float bend) {
    P d = sub(target, root);
    float dist = std::clamp(len(d), .001f, l1 + l2 - .01f);
    P u = scale(d, 1 / std::max(len(d), .001f));
    float a = (l1 * l1 - l2 * l2 + dist * dist) / (2 * dist);
    float h = std::sqrt(std::max(0.0f, l1 * l1 - a * a));
    P base = add(root, scale(u, a));
    return add(base, scale(P{-u.y, u.x}, h * bend));
}

// Lighting of the moment, shared by everything on stage.
struct Light {
    SDL_Color key, fill; // key from the truss, fill from the floor wash
    float level;         // overall brightness
};

void limb(P a, P b, float w, SDL_Color c, const Light &l) {
    thickLine(a.x, a.y, b.x, b.y, w, c);
    disc(a.x, a.y, w / 2, w / 2, c, c, 12);
    disc(b.x, b.y, w / 2, w / 2, c, c, 12);
    // Rim light: the truss is above and to the left of everyone.
    const SDL_Color rim = alpha(l.key, .28f * l.level);
    thickLine(a.x - w * .22f, a.y - w * .15f, b.x - w * .22f, b.y - w * .15f, w * .18f, rim);
}
void blob(P c, float rx, float ry, SDL_Color col, const Light &l) {
    disc(c.x, c.y, rx, ry, col, shade(col, .25f), 18);
    disc(c.x - rx * .3f, c.y - ry * .35f, rx * .45f, ry * .35f, alpha(l.key, .35f * l.level),
         alpha(l.key, 0), 14);
}

// ------------------------------------------------------------ a person
struct Look {
    SDL_Color skin, shirt, sleeve, pants, shoes, hair;
    int hairStyle; // 0 short spiky, 1 long, 2 beanie
};
struct Body {
    P hip, chest, neck, head, shoulderL, shoulderR;
    float s;
};
// Feet at `feet`, `s` = scale (1 = about 250 px tall), lean in radians
// (+ = toward screen right), `crouch` in px of knee bend, `nod` head tilt.
Body pose(P feet, float s, float lean, float crouch, float nod) {
    Body b;
    b.s = s;
    b.hip = {feet.x, feet.y - (118 - crouch) * s};
    P up = dir(Pi + lean);
    b.chest = add(b.hip, scale(up, 50 * s));
    b.neck = add(b.hip, scale(up, 86 * s));
    b.head = add(b.neck, scale(dir(Pi + lean + nod), 26 * s));
    const P side = {24 * s * std::cos(lean), -24 * s * std::sin(lean)}; // across the shoulders
    b.shoulderL = sub(add(b.hip, scale(up, 80 * s)), side);
    b.shoulderR = add(add(b.hip, scale(up, 80 * s)), side);
    return b;
}
void legs(const Body &b, P feet, float stance, const Look &lk, const Light &l) {
    const float s = b.s;
    for (int side : {-1, 1}) {
        P hip = {b.hip.x + side * 11 * s, b.hip.y};
        P foot = {feet.x + side * stance * s, feet.y};
        P knee = joint(hip, foot, 60 * s, 62 * s, side < 0 ? 1.f : -1.f);
        limb(hip, knee, 27 * s, lk.pants, l);
        limb(knee, foot, 22 * s, shade(lk.pants, .12f), l);
        disc(foot.x + side * 7 * s, foot.y - 3 * s, 16 * s, 8 * s, lk.shoes, shade(lk.shoes, .3f), 14);
    }
}
void torso(const Body &b, const Look &lk, const Light &l) {
    const float s = b.s;
    // A tapered trunk: hips, waist, chest.
    P up = sub(b.neck, b.hip);
    float ul = std::max(len(up), .001f);
    P n = {-up.y / ul, up.x / ul};
    auto at = [&](float t, float w) { return std::pair<P, P>{add(lerp(b.hip, b.neck, t), scale(n, w * s)),
                                                            sub(lerp(b.hip, b.neck, t), scale(n, w * s))}; };
    auto [h1, h2] = at(0, 24);
    auto [w1, w2] = at(.45f, 22);
    auto [c1, c2] = at(.95f, 30);
    quad(h1, w1, w2, h2, lk.pants, lk.shirt, lk.shirt, lk.pants);
    quad(w1, c1, c2, w2, lk.shirt, shade(lk.shirt, .1f), shade(lk.shirt, .1f), lk.shirt);
    thickLine(c2.x, c2.y, w2.x, w2.y, 3 * s, alpha(l.key, .3f * l.level)); // rim on the lit side
    limb(b.neck, lerp(b.neck, b.head, .4f), 9 * s, shade(lk.skin, .2f), l);
}
void head(const Body &b, const Look &lk, const Light &l, float swing) {
    const float s = b.s;
    P h = b.head;
    if (lk.hairStyle == 1) { // long hair, flying with the headbang
        for (int side : {-1, 1})
            for (int i = 0; i < 3; ++i) {
                P root = {h.x + side * (8 + i * 4) * s, h.y - 8 * s};
                P tip = {h.x + side * (17 + i * 5) * s + swing * 16 * s, h.y + (40 - i * 4) * s - std::abs(swing) * 12 * s};
                limb(root, tip, 9 * s, lk.hair, l);
            }
    }
    blob(h, 15 * s, 18 * s, lk.skin, l);
    if (lk.hairStyle == 0) { // short, spiky, bleached
        disc(h.x, h.y - 9 * s, 16 * s, 11 * s, lk.hair, shade(lk.hair, .15f), 16);
        for (int i = 0; i < 5; ++i) {
            float a = Pi + (i - 2) * .38f;
            P root = add(h, scale(dir(a), 12 * s));
            P tip = add(h, scale(dir(a + swing * .2f + .25f), 22 * s));
            limb(root, tip, 7 * s, lk.hair, l);
        }
    } else if (lk.hairStyle == 1) {
        disc(h.x, h.y - 9 * s, 16 * s, 11 * s, lk.hair, shade(lk.hair, .2f), 16);
    } else { // beanie
        disc(h.x, h.y - 8 * s, 16 * s, 13 * s, lk.hair, shade(lk.hair, .25f), 16);
        thickLine(h.x - 16 * s, h.y - 3 * s, h.x + 16 * s, h.y - 3 * s, 5 * s, shade(lk.hair, -.3f));
    }
    // Dark lenses: nobody needs to see eyes this far back, and it reads as cool.
    thickLine(h.x - 9 * s, h.y + 1 * s, h.x + 9 * s, h.y + 1 * s, 4 * s, rgb(12, 12, 16));
}
void arm(P shoulder, P hand, float bend, const Body &b, const Look &lk, const Light &l) {
    const float s = b.s;
    P elbow = joint(shoulder, hand, 44 * s, 44 * s, bend);
    limb(shoulder, elbow, 19 * s, lk.sleeve, l);
    limb(elbow, hand, 15 * s, lk.skin, l);
    disc(hand.x, hand.y, 9 * s, 9 * s, lk.skin, shade(lk.skin, .2f), 10);
}

// ------------------------------------------------------------ the set
void backdrop(const Cues &c, const Light &l, float energy) {
    const SDL_Color top = rgb(10, 10, 16), floorBack = rgb(24, 22, 28), floorFront = rgb(12, 11, 14);
    quad({0, 0}, {W, 0}, {W, 300}, {0, 300}, top, top, shade(l.fill, .82f), shade(l.fill, .82f));
    // LED wall behind the drum riser: an equaliser that rides the beat.
    const float ph = frac(c.beat);
    const int cols = 30, rows = 9;
    const float x0 = 300, y0 = 44, cw = 680.f / cols, ch = 220.f / rows;
    rect(x0 - 6, y0 - 6, 680 + 12, 220 + 12, rgb(4, 4, 6));
    for (int i = 0; i < cols; ++i) {
        float wave = .5f + .5f * std::sin(float(c.beat) * Pi * .5f + i * .55f);
        float hgt = c.failed ? 0 : (.15f + .75f * energy * wave) * (.75f + .25f * (1 - ph));
        int lit = int(std::round(hgt * rows));
        for (int r = 0; r < rows; ++r) {
            bool on = rows - r <= lit;
            SDL_Color col = on ? alpha(mix(l.key, rgb(255, 255, 255), r == rows - lit ? .4f : 0), .28f + .3f * energy)
                               : rgb(14, 14, 20, 255);
            rect(x0 + i * cw + 1, y0 + r * ch + 1, cw - 2, ch - 2, col);
        }
    }
    // Stage floor in perspective, toward the far end of the highway.
    quad({0, 300}, {W, 300}, {W, 440}, {0, 440}, floorBack, floorBack, floorFront, floorFront);
    for (int i = -8; i <= 8; ++i) {
        float xb = 640 + i * 70.f, xf = 640 + i * 150.f;
        thickLine(xb, 300, xf, 440, 1.5f, rgb(0, 0, 0, 90));
    }
    // The lip of the stage and the pit below it.
    rect(0, 438, W, 6, alpha(l.key, .35f * l.level));
    quad({0, 444}, {W, 444}, {W, H}, {0, H}, rgb(8, 8, 10), rgb(8, 8, 10), rgb(3, 3, 4), rgb(3, 3, 4));
}
void truss(const Cues &c, const Light &l, float energy, std::array<P, 5> &lamps) {
    const SDL_Color metal = rgb(70, 72, 80);
    for (float y : {14.f, 30.f})
        thickLine(250, y, 1030, y, 3, metal);
    for (int i = 0; i <= 26; ++i) {
        float x = 250 + i * 30.f;
        thickLine(x, 14, x + 15, 30, 2, metal);
        thickLine(x + 15, 30, x + 30, 14, 2, metal);
    }
    const float xs[5] = {330, 470, 640, 810, 950};
    for (int i = 0; i < 5; ++i) {
        // Beams swing in time: odd lamps against even ones.
        float swing = (c.failed ? 0 : .32f * energy) * std::sin(float(c.beat) * Pi * .5f + (i % 2) * Pi);
        P lamp = {xs[i], 40};
        lamps[size_t(i)] = lamp;
        P spot = add(lamp, scale(dir(swing + (i - 2) * .08f), 400));
        P n = {-(spot.y - lamp.y), spot.x - lamp.x};
        float nl = std::max(len(n), .001f);
        n = scale(n, 1 / nl);
        float strobe = c.power ? (frac(c.beat * 2) < .5f ? 1.f : .55f) : 1.f;
        SDL_Color beam = alpha(l.key, (.10f + .14f * energy) * strobe * l.level);
        quad(add(lamp, scale(n, 6)), sub(lamp, scale(n, 6)), sub(spot, scale(n, 70)), add(spot, scale(n, 70)), beam,
             beam, alpha(beam, 0), alpha(beam, 0));
        glow(spot.x, spot.y, 260, 70, alpha(l.key, .25f * l.level * strobe));
        rect(lamp.x - 9, lamp.y - 10, 18, 14, rgb(30, 30, 36));
        glow(lamp.x, lamp.y + 4, 46, 46, alpha(mix(l.key, rgb(255, 255, 255), .5f), .9f * l.level));
    }
}
void crowd(const Cues &c, const Light &l, float energy) {
    // Two rows of silhouettes in the pit. Hands go up as the band earns them.
    for (int row = 0; row < 2; ++row) {
        const int count = row ? 15 : 19;
        const float baseY = row ? 708.f : 652.f, r = row ? 24.f : 17.f;
        for (int i = 0; i < count; ++i) {
            const uint32_t id = uint32_t(row * 100 + i);
            float x = (i + .5f) * W / count + (hash01(id * 3) - .5f) * 40;
            float bob = std::pow(1 - frac(c.beat + hash01(id * 5) * .2), 3.0f) * 7 * energy;
            float y = baseY + (hash01(id * 7) - .5f) * 14 + bob;
            const SDL_Color body = row ? rgb(14, 13, 18) : rgb(22, 20, 27);
            bool hands = !c.failed && hash01(id * 11) < energy * 1.1f - .1f;
            if (hands) {
                float wave = std::sin(float(c.beat) * Pi + hash01(id * 13) * 6) * .25f;
                for (int side : {-1, 1}) {
                    if (side > 0 && hash01(id * 17) < .5f)
                        continue;
                    P sh = {x + side * r * .8f, y + r * .6f};
                    P hand = add(sh, scale(dir(Pi + side * (.35f + wave)), r * 3.2f));
                    thickLine(sh.x, sh.y, hand.x, hand.y, r * .45f, body);
                    disc(hand.x, hand.y, r * .3f, r * .3f, body, body, 8);
                }
            }
            disc(x, y + r * 1.6f, r * 1.7f, r * 1.1f, body, body, 18);
            disc(x, y, r * .8f, r, body, body, 16);
            disc(x, y - r * .45f, r * .75f, r * .45f, alpha(l.key, .55f * l.level), alpha(l.key, 0), 12);
        }
    }
}
void sparks(const Cues &c) {
    const double age = c.time - mem.powerStart;
    if (age < 0 || age > 1.8)
        return;
    for (float baseX : {300.f, 980.f})
        for (uint32_t i = 0; i < 36; ++i) {
            float t = float(age) - hash01(i * 3 + uint32_t(baseX)) * .5f;
            if (t < 0)
                continue;
            float vx = (hash01(i * 7) - .5f) * 140, vy = -380 - hash01(i * 11) * 260;
            float x = baseX + vx * t, y = 440 + vy * t + 520 * t * t;
            float life = 1 - t / 1.3f;
            if (life <= 0 || y > 446)
                continue;
            glow(x, y, 10 + 8 * life, 10 + 8 * life, rgb(255, 210, 120, int(220 * life)));
        }
}

// ------------------------------------------------------------ the band
const Look nina{rgb(232, 196, 170), rgb(46, 92, 60), rgb(52, 104, 68), rgb(44, 44, 56), rgb(220, 220, 225),
                rgb(238, 236, 214), 0};
const Look caio{rgb(196, 146, 112), rgb(150, 34, 38), rgb(122, 28, 34), rgb(46, 62, 98), rgb(34, 34, 38),
                rgb(26, 20, 18), 1};
const Look bento{rgb(150, 104, 78), rgb(210, 200, 180), rgb(150, 104, 78), rgb(36, 36, 40), rgb(30, 30, 30),
                 rgb(200, 120, 30), 2};

void singer(const Cues &c, const Light &l, float energy) {
    const P feet = {360, 420};
    const float s = .92f, ph = frac(c.beat);
    const float nod = (c.failed ? .35f : .06f + .22f * energy * std::pow(1 - ph, 3.0f));
    const float crouch = c.failed ? 18 : 4 + 9 * energy * std::pow(1 - ph, 2.0f);
    Body b = pose(feet, s, c.failed ? .12f : .03f, crouch, nod);
    // Mic stand in front, a little to her left.
    const P base = {feet.x + 22 * s, feet.y}, mic = {b.head.x + 16 * s, b.head.y + 16 * s};
    legs(b, feet, 20, nina, l);
    torso(b, nina, l);
    head(b, nina, l, 0);
    thickLine(base.x, base.y, base.x, mic.y + 30 * s, 3, rgb(60, 60, 66));
    thickLine(base.x, mic.y + 30 * s, mic.x, mic.y, 3, rgb(60, 60, 66));
    disc(base.x, base.y, 18 * s, 4 * s, rgb(40, 40, 44), rgb(20, 20, 24), 12);
    disc(mic.x, mic.y, 5 * s, 5 * s, rgb(90, 90, 96), rgb(40, 40, 44), 10);
    arm(b.shoulderR, add(mic, {2 * s, 6 * s}), 1, b, nina, l);
    // Free arm: fist pumps on the beat once the show is going, both up in star power.
    P hand;
    if (c.failed)
        hand = add(b.shoulderL, {-6 * s, 80 * s});
    else if (c.power || energy > .55f) {
        float pump = c.power ? 1 : (ph < .5f ? 1 - ph * 2 : 0);
        hand = add(b.shoulderL, {-30 * s, (20 - 100 * pump) * s});
    } else
        hand = add(b.shoulderL, {-24 * s, 58 * s + 6 * std::sin(float(c.beat) * Pi) * s});
    arm(b.shoulderL, hand, 1, b, nina, l);
}

void guitarist(const Cues &c, const Light &l, float energy) {
    const float s = 1.0f, ph = frac(c.beat);
    const float hit = ease(c.time - mem.lastHit, .14), miss = ease(c.time - mem.lastMiss, .5);
    float jump = 0;
    if (c.power) // hops every other beat
        jump = std::sin(Pi * frac(c.beat / 2)) * 34 * (frac(c.beat / 2) < .5f ? 1.f : 0.f);
    const P feet = {960, 404 - jump};
    const float nod = c.failed ? .4f : (.05f + .32f * energy * std::pow(1 - ph, 3.0f)) - .3f * miss;
    const float crouch = c.failed ? 20 : 6 + 12 * energy * std::pow(1 - ph, 2.0f) + (c.power ? 10 : 0);
    const float lean = c.failed ? .15f : -.06f - .18f * miss;
    Body b = pose(feet, s, lean, crouch, nod);
    legs(b, {feet.x, 404}, c.power ? 30 : 24, caio, l);
    torso(b, caio, l);
    head(b, caio, l, (c.failed ? 0 : (1 - ph) * energy) - miss);
    // The guitar: body at his right hip (screen left), neck up toward screen right.
    const float neckAngle = c.failed ? -.05f : (c.power ? .85f : .5f + .06f * std::sin(float(c.beat) * Pi * .5f));
    const P body = add(b.hip, {-12 * s, -18 * s});
    const P neckDir = {std::cos(neckAngle), -std::sin(neckAngle)};
    const P nut = add(body, scale(neckDir, 118 * s)), headEnd = add(body, scale(neckDir, 142 * s));
    const SDL_Color finish = rgb(40, 170, 160), edge = rgb(18, 70, 66);
    thickLine(body.x, body.y, nut.x, nut.y, 8 * s, rgb(70, 46, 28));
    thickLine(nut.x, nut.y, headEnd.x, headEnd.y, 11 * s, rgb(24, 24, 28));
    // An offset body: two lobes and a horn, not any real maker's shape.
    disc(body.x - 6 * s, body.y + 8 * s, 30 * s, 24 * s, finish, edge, 22);
    disc(body.x + 12 * s, body.y - 10 * s, 20 * s, 16 * s, finish, edge, 18);
    thickLine(body.x + 14 * s, body.y - 18 * s, body.x + 30 * s, body.y - 30 * s, 9 * s, finish);
    disc(body.x - 2 * s, body.y + 2 * s, 9 * s, 9 * s, rgb(240, 240, 236), rgb(200, 200, 196), 12); // pickguard
    for (int i = 0; i < 3; ++i) // strings catching the light
        thickLine(body.x - 14 * s, body.y + (6 - i * 3) * s, nut.x, nut.y + (2 - i * 2) * s * .5f, 1,
                  alpha(rgb(255, 255, 255), .25f * l.level));
    glow(body.x, body.y, 90 * s, 70 * s, alpha(finish, .25f * l.level));
    // Fret hand follows the frets held; strum hand snaps down on each hit.
    int lane = -1;
    for (int i = 0; i < 5; ++i)
        if (c.held & (1 << i)) {
            lane = i;
            break;
        }
    const float along = lane < 0 ? .66f : .9f - lane * .07f;
    const P fret = add(add(body, scale(neckDir, 118 * s * along)), {0, 4 * s});
    const P pick = add(body, {-4 * s, (-10 + 26 * hit) * s});
    arm(b.shoulderR, fret, -1, b, caio, l);
    arm(b.shoulderL, pick, 1, b, caio, l);
    if (miss > 0) // a red flash on a fluffed note
        glow(b.head.x, b.head.y, 120, 120, rgb(255, 40, 40, int(120 * miss)));
}

void drummer(const Cues &c, const Light &l, float energy) {
    const float s = .55f, ph2 = frac(c.beat * 2);
    const int beatNo = int(std::floor(c.beat));
    const P seat = {640, 140};
    // Kit first: cymbals behind him, drums in front.
    const float crashAge = float(c.beat) - std::floor(float(c.beat) / 4) * 4;
    const float wob = c.failed ? 0 : ease(crashAge, .9) * energy;
    auto cymbal = [&](P at, float r, float tilt) {
        thickLine(at.x, at.y, at.x, at.y + 60, 2, rgb(60, 60, 66));
        disc(at.x, at.y, r * (1 + wob * .1f), r * (.28f + tilt), rgb(220, 180, 90), rgb(120, 90, 40), 18);
    };
    cymbal({580, 92}, 22, .02f);
    cymbal({708, 80}, 26, wob * .2f);
    Body b = pose({seat.x, seat.y + 118 * s}, s, 0, 0, .05f + .25f * energy * std::pow(1 - frac(c.beat), 3.0f));
    torso(b, bento, l);
    head(b, bento, l, 0);
    disc(612, 112, 16, 6, rgb(200, 196, 190), rgb(120, 116, 110), 14); // snare
    disc(628, 98, 14, 9, rgb(150, 30, 40), rgb(80, 16, 20), 14);       // toms
    disc(656, 98, 14, 9, rgb(150, 30, 40), rgb(80, 16, 20), 14);
    disc(640, 150, 42, 42, rgb(150, 30, 40), rgb(70, 14, 18), 26); // bass drum, under the highway's end
    disc(640, 150, 33, 33, rgb(30, 30, 34), rgb(16, 16, 18), 24);
    // Sticks: right hand rides the hats in eighths, left hits the snare on 2 and 4,
    // and the crash gets the downbeat of each bar.
    const float lift = c.failed ? .2f : std::sin(Pi * ph2) * (.4f + .6f * energy);
    const bool backbeat = beatNo % 2 == 1 && ph2 < .5f;
    const bool crash = beatNo % 4 == 0 && frac(c.beat) < .5f && energy > .45f;
    P hatT = crash ? P{708, 82} : P{584, 96};
    P snareT = {612, 108};
    P handR = add(lerp(b.shoulderL, hatT, .9f), {0, -lift * 26});
    P handL = add(lerp(b.shoulderR, snareT, .9f), {0, -(backbeat ? lift : lift * .3f) * 26});
    for (auto [sh, hd, bend] : {std::tuple{b.shoulderL, handR, 1.f}, std::tuple{b.shoulderR, handL, -1.f}}) {
        P elbow = joint(sh, hd, 44 * s, 44 * s, bend);
        limb(sh, elbow, 15 * s, bento.sleeve, l);
        limb(elbow, hd, 12 * s, bento.skin, l);
        P d = sub(hd, elbow);
        P tip = add(hd, scale(d, 30 * s / std::max(len(d), .001f)));
        thickLine(hd.x, hd.y, tip.x, tip.y, 3, rgb(220, 200, 160));
    }
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
        if (f == "power" && mem.powerStart < -1e8)
            mem.powerStart = c.time - .5, mem.power = true;
    }
#endif
    // Track what happened since the last frame; a restart rewinds the clock.
    if (c.time < mem.lastTime - .25 || c.hits < mem.hits || c.misses < mem.misses)
        reset();
    if (c.hits > mem.hits)
        mem.lastHit = c.time;
    if (c.misses > mem.misses)
        mem.lastMiss = c.time;
    if (c.power && !mem.power)
        mem.powerStart = c.time;
    mem.hits = c.hits, mem.misses = c.misses, mem.power = c.power, mem.lastTime = c.time;

    // How hard the show is going: the multiplier and rock meter build it, star power maxes it.
    float energy = clamp01(.2f + .17f * float(c.multiplier - 1) + .45f * float(c.meter - .4));
    if (c.power)
        energy = 1;
    if (c.failed)
        energy = 0;
    const bool danger = !c.failed && c.meter < c.meterRed;
    Light l;
    if (c.power)
        l = {rgb(110, 200, 255), rgb(40, 90, 160), 1};
    else if (danger)
        l = {rgb(255, 60, 50), rgb(110, 20, 20), .7f};
    else if (c.failed)
        l = {rgb(120, 120, 140), rgb(20, 20, 30), .35f};
    else {
        // Warm amber and magenta take turns every four bars.
        const float t = .5f + .5f * std::sin(float(c.beat) * Pi / 16);
        l = {mix(rgb(255, 170, 70), rgb(240, 70, 170), t), mix(rgb(120, 60, 30), rgb(100, 30, 90), t), 1};
    }
    std::array<P, 5> lamps{};
    backdrop(c, l, energy);
    drummer(c, l, energy);
    truss(c, l, energy, lamps);
    singer(c, l, energy);
    guitarist(c, l, energy);
    sparks(c);
    crowd(c, l, energy);
}
} // namespace fret::stage
