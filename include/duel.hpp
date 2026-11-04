#pragma once
// Rival duels: one on one against an original rival guitarist on the same
// chart. Star phrases arm attacks instead of star power; each side throws them
// at the other. The rival is simulated note by note with a skill that drops
// while it is rattled by one of the player's attacks. Whoever runs their rock
// meter dry first loses; at the end of the song the fuller meter wins.
#include "song.hpp"
#include <cstdint>
#include <vector>

namespace fret::duel {
enum class Attack { None, Lefty, Speed, Blackout, String };
const char *attackName(Attack a); // English; translated for display

struct Rival {
    const char *name, *title, *taunt; // title and taunt in English; translated for display
    double skill;                       // chance to hit a note when calm
    unsigned color;                     // 0xRRGGBB, their lights on stage
    bool bandOnly = false;              // fought by two to four players only
};
// Three career rivals, weakest first, then Lived: the band career's final boss.
const std::vector<Rival> &rivals();
constexpr int finalBoss = 3;
// An attack picked by `salt`, the same way a duel picks one.
Attack pickAttack(uint64_t salt);

struct Effect {
    Attack kind = Attack::None;
    double from = -1e9, until = -1e9;
    int lane = 0, presses = 0; // String: the dead lane and the presses still needed
};

class Duel {
  public:
    static constexpr double meterStep = .018, attackDamage = .16, attackLife = 7, rattledSkill = .6;
    static constexpr int stringPresses = 6;

    Duel(const Track &track, int rival, uint32_t seed);
    // Plays the rival through every note up to `time`, fires its attacks, and
    // repairs a broken string from the player's new fret presses.
    void update(double time, uint8_t pressed);
    // The player's attack, if one is armed. Returns what was thrown.
    Attack fire(double time);
    // The frets the player is holding, minus a broken string.
    uint8_t mask(uint8_t frets, double time) const;
    bool suffering(Attack a, double time) const { return onPlayer.kind == a && time < onPlayer.until; }
    bool rattled(double time) const { return time < rivalRattledUntil; }
    bool rivalOut() const { return rivalMeter <= 0; }

    int rival;
    int armed = 0;            // attacks the player holds, at most two
    double rivalMeter = .5;
    int rivalHits = 0, rivalMisses = 0;
    double rivalHitAt = -1e9, rivalMissAt = -1e9;
    Effect onPlayer;          // the attack the player is under
    Attack thrown = Attack::None, received = Attack::None;
    double thrownAt = -1e9, receivedAt = -1e9, rivalRattledUntil = -1e9;

  private:
    const Track *track;
    size_t next = 0;
    std::vector<int> phraseLeft;
    std::vector<int> phraseMisses; // the rival keeps a phrase with one slip at most
    int rivalArmed = 0;
    double rivalFireAt = 1e18;
    uint32_t seed;
    double roll(uint32_t salt) const;
    Attack pick(uint32_t salt) const;
};
} // namespace fret::duel
