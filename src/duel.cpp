#include "duel.hpp"
#include <algorithm>

namespace fret::duel {
const char *attackName(Attack a) {
    switch (a) {
    case Attack::Lefty:
        return "LEFTY FLIP";
    case Attack::Speed:
        return "OVERDRIVE";
    case Attack::Blackout:
        return "BLACKOUT";
    case Attack::String:
        return "BROKEN STRING";
    default:
        return "";
    }
}

const std::vector<Rival> &rivals() {
    // Original characters; none of them is, or is meant to suggest, a real musician.
    static const std::vector<Rival> r = {
        {"Dona Rubi", "the blues queen of the late shift", "Sit down, kid. I was bending strings before you were born.",
         .80, 0xd2283c},
        {"Viktor Kade", "the shred king in the leopard jacket", "Six strings? Cute. I brought two necks.", .86, 0xffb428},
        {"MK-77", "the machine that never misses a beat", "PROBABILITY OF YOUR VICTORY: 0.3 PERCENT.", .91, 0x3cdcff},
        // An eighties album-cover devil, played for laughs: his name is the word backwards.
        {"Lived", "the devil of eighties metal, spelled backwards", "Shout all you want, mortals. Nobody leaves my show.",
         .93, 0xff3a14, true},
    };
    return r;
}

Attack pickAttack(uint64_t salt) {
    uint64_t x = salt + 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    static const Attack all[4] = {Attack::Lefty, Attack::Speed, Attack::Blackout, Attack::String};
    return all[x % 4];
}

Duel::Duel(const Track &t, int r, uint32_t s)
    : rival(std::clamp(r, 0, int(rivals().size()) - 1)), track(&t), phraseLeft(t.phrases.size()),
      phraseMisses(t.phrases.size(), 0), seed(s) {
    for (const auto &n : t.notes)
        if (n.phrase >= 0 && size_t(n.phrase) < phraseLeft.size())
            ++phraseLeft[size_t(n.phrase)];
}

double Duel::roll(uint32_t salt) const {
    // splitmix64's finaliser: neighbouring salts give unrelated rolls.
    uint64_t x = (uint64_t(seed) << 32 | salt) + 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    return double(x >> 11) / double(1ull << 53);
}
Attack Duel::pick(uint32_t salt) const {
    static const Attack all[4] = {Attack::Lefty, Attack::Speed, Attack::Blackout, Attack::String};
    return all[size_t(roll(salt) * 4) % 4];
}

void Duel::update(double time, uint8_t pressed) {
    // The rival plays every note that has reached the strike line.
    const double calm = rivals()[size_t(rival)].skill;
    while (next < track->notes.size() && track->notes[next].time <= time) {
        const auto &n = track->notes[next];
        const double skill = n.time < rivalRattledUntil ? rattledSkill : calm;
        const bool hit = roll(uint32_t(next) * 2 + 1) < skill;
        if (hit)
            ++rivalHits, rivalHitAt = n.time, rivalMeter = std::min(1.0, rivalMeter + meterStep);
        else
            ++rivalMisses, rivalMissAt = n.time, rivalMeter = std::max(0.0, rivalMeter - meterStep * 3);
        if (n.phrase >= 0 && size_t(n.phrase) < phraseLeft.size()) {
            const auto p = size_t(n.phrase);
            phraseMisses[p] += hit ? 0 : 1;
            if (--phraseLeft[p] == 0 && phraseMisses[p] <= 1) {
                rivalArmed = std::min(2, rivalArmed + 1);
                // A beat or three to savour it before throwing.
                rivalFireAt = std::min(rivalFireAt, n.time + 1.5 + 2.5 * roll(uint32_t(next) * 7 + 3));
            }
        }
        ++next;
    }
    // The rival throws when armed and the player is not already under an attack.
    if (rivalArmed > 0 && time >= rivalFireAt && time >= onPlayer.until) {
        --rivalArmed;
        Effect e;
        e.kind = pick(uint32_t(time * 10) + 11);
        e.from = time, e.until = time + attackLife;
        if (e.kind == Attack::String) {
            e.lane = int(roll(uint32_t(time * 10) + 13) * 3); // green, red or yellow: every difficulty uses them
            e.presses = stringPresses;
            e.until = time + attackLife * 1.5; // it ends sooner if the player repairs it
        }
        onPlayer = e;
        received = e.kind, receivedAt = time;
        rivalFireAt = rivalArmed > 0 ? time + attackLife + 4 : 1e18;
    }
    // Mashing the dead fret restrings it.
    if (onPlayer.kind == Attack::String && time < onPlayer.until && (pressed & (1 << onPlayer.lane)))
        if (--onPlayer.presses <= 0)
            onPlayer.until = time;
}

Attack Duel::fire(double time) {
    if (armed <= 0)
        return Attack::None;
    --armed;
    thrown = pick(uint32_t(time * 10) + 29), thrownAt = time;
    rivalMeter = std::max(0.0, rivalMeter - attackDamage);
    rivalRattledUntil = time + attackLife;
    return thrown;
}

uint8_t Duel::mask(uint8_t frets, double time) const {
    if (onPlayer.kind == Attack::String && time < onPlayer.until)
        frets = uint8_t(frets & ~(1 << onPlayer.lane));
    return frets;
}
} // namespace fret::duel
