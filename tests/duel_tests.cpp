// Rival duels: the simulated rival, attacks both ways, and the career gate.
#include "career.hpp"
#include "duel.hpp"
#include <cstdlib>
#include <iostream>
#include <string>

using namespace fret;
using namespace fret::duel;

namespace {
int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}
// Three minutes at four notes a second, with an eight-note star phrase every 15 seconds.
Track chart() {
    Track t;
    t.instrument = "Guitar";
    for (int i = 0; i < 720; ++i) {
        Note n;
        n.time = 2 + i * .25;
        n.mask = uint8_t(1 << (i % 5));
        const int inCycle = i % 60;
        if (inCycle < 8) {
            n.phrase = int(t.phrases.size()) - (inCycle == 0 ? 0 : 1);
            if (inCycle == 0)
                t.phrases.push_back({0, 0});
        }
        t.notes.push_back(n);
    }
    return t;
}
// Runs a duel to the end with the player throwing every attack as soon as it is armed
// (one per star phrase). Returns the time the rival dropped, or a negative value.
double beatenAt(const Track &t, int rival, uint32_t seed) {
    Duel d(t, rival, seed);
    size_t phrase = 0;
    for (double time = 0; time < 185; time += 1.0 / 60) {
        d.update(time, 0);
        if (phrase < t.phrases.size() && time > 2 + phrase * 15 + 2.2) // the player finished a phrase
            ++phrase, d.armed = std::min(2, d.armed + 1);
        d.fire(time);
        if (d.rivalOut())
            return time;
    }
    return -1;
}
} // namespace

int main() {
    const Track t = chart();
    check(rivals().size() == 4, "three rivals and the final boss");
    check(rivals()[0].skill < rivals()[1].skill && rivals()[1].skill < rivals()[2].skill, "rivals get harder");
    check(rivals()[finalBoss].bandOnly && !rivals()[0].bandOnly && rivals()[finalBoss].skill > rivals()[2].skill,
          "Lived is the hardest and fights bands only");
    check(std::string(rivals()[finalBoss].name) == "Lived", "the final boss is Lived");
    {
        bool seen[5] = {};
        for (uint64_t salt = 0; salt < 64; ++salt)
            seen[int(pickAttack(salt))] = true;
        check(!seen[0] && seen[1] && seen[2] && seen[3] && seen[4], "battles pick every attack");
    }

    {
        Duel calm(t, 0, 1);
        calm.update(60, 0);
        check(calm.rivalHits + calm.rivalMisses == 233, "the rival plays every note that has passed");
        check(calm.rivalMeter > .5, "a calm rival fills their meter");
        check(calm.received != Attack::None, "the rival throws attacks from its star phrases");
        Duel again(t, 0, 1);
        again.update(60, 0);
        check(again.rivalHits == calm.rivalHits && again.received == calm.received, "a duel replays the same");
    }
    {
        Duel d(t, 1, 7);
        d.update(10, 0);
        const double before = d.rivalMeter;
        check(d.fire(10) == Attack::None, "nothing to throw without an armed attack");
        d.armed = 1;
        check(d.fire(10) != Attack::None && d.armed == 0, "an armed attack is thrown once");
        check(d.rivalMeter < before - Duel::attackDamage + 1e-9 && d.rattled(12) && !d.rattled(18),
              "an attack hurts and rattles the rival for a while");
        Duel calm(t, 1, 7);
        calm.update(17, 0);
        d.update(17, 0);
        check(d.rivalMisses > calm.rivalMisses, "a rattled rival misses more");
    }
    {
        Duel d(t, 2, 3);
        d.onPlayer = {Attack::String, 0, 100, 1, Duel::stringPresses};
        check(d.mask(0b11111, 5) == 0b11101, "a broken string kills its fret");
        check(d.suffering(Attack::String, 5), "the player knows it is under the attack");
        for (int i = 0; i < Duel::stringPresses; ++i)
            d.update(5 + i * .1, 0b00010);
        check(d.mask(0b11111, 6) == 0b11111 && !d.suffering(Attack::String, 6), "mashing the fret restrings it");
    }
    // Every rival can be beaten by a player who lands every phrase and throws
    // each attack; the harder the rival, the longer it holds out.
    double times[3];
    for (int r = 0; r < 3; ++r) {
        times[r] = beatenAt(t, r, 42);
        check(times[r] > 0, "an attacking player can beat every rival");
    }
    check(times[0] <= times[2], "the machine holds out longer than the blues queen");
    {
        // A player who never attacks loses on the meter at the end.
        Duel d(t, 0, 5);
        d.update(185, 0);
        check(!d.rivalOut() && d.rivalMeter > .5, "a passive player cannot win");
    }

    // The career: a duel after the encore of the big venues gates the next one.
    std::vector<career::Tier> tiers(8);
    for (int v = 0; v < 8; ++v) {
        tiers[size_t(v)].venue = v;
        for (int s = 0; s < 5; ++s)
            tiers[size_t(v)].songs.push_back({"v" + std::to_string(v) + "s" + std::to_string(s), "Song", "Band", v});
    }
    career::assignDuels(tiers);
    check(tiers[3].rival == 0 && tiers[5].rival == 1 && tiers[7].rival == 2 && tiers[0].rival < 0,
          "duels at the club, the festival and the stadium");
    check(tiers[3].duel.folder == tiers[3].songs.back().folder, "a duel is fought over the encore");
    career::Career c;
    c.active = true, c.difficulty = 3, c.tiers = tiers;
    Scores scores;
    for (size_t v = 0; v < 4; ++v)
        for (const auto &g : tiers[v].songs)
            scores.submit(g.folder, "Guitar", 3, {1000, 4, 90, false});
    check(c.duelOpen(scores, 3), "the encore opens the duel");
    check(!c.tierOpen(scores, 4), "the next venue waits for the duel");
    c.beaten.insert(0);
    check(c.tierOpen(scores, 4), "beating the rival opens the next venue");
    check(career::Career::duelPrize(2) > career::Career::duelPrize(0), "bigger rivals pay more");

    if (failures)
        return EXIT_FAILURE;
    std::cout << "duel: all checks passed (rivals fall at " << int(times[0]) << "s, " << int(times[1]) << "s, "
              << int(times[2]) << "s)\n";
    return EXIT_SUCCESS;
}
