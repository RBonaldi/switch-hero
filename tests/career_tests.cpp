// Career mode rules: setlists, unlocks, cash and the save file.
#include "career.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>

using namespace fret;
using namespace fret::career;

namespace {
int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}
} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "switch-hero-career-test";
    fs::remove_all(root);
    fs::create_directories(root);
    // Forty songs rated 0..6 in a scrambled order, plus two without a rating.
    std::vector<std::pair<fs::path, std::pair<std::string, std::string>>> pool;
    for (int i = 0; i < 42; ++i) {
        const fs::path folder = root / ("song" + std::to_string(i));
        fs::create_directories(folder);
        std::ofstream ini(folder / "song.ini");
        ini << "[song]\nname = Song " << i << "\n";
        if (i < 40)
            ini << "diff_guitar = " << (i * 7) % 7 << "\n"; // spread over 0..6 below
        pool.push_back({folder, {"Song " + std::to_string(i), "Band"}});
    }
    for (int i = 0; i < 40; ++i) {
        std::ofstream ini(root / ("song" + std::to_string(i)) / "song.ini");
        ini << "[song]\nname = Song " << i << "\ndiff_guitar = " << (i * 3) % 7 << "\n";
    }
    check(ratingOf(root / "song0") == 0 && ratingOf(root / "song1") == 3, "song.ini ratings are read");
    check(ratingOf(root / "song41") == -1, "a missing rating is -1");

    const auto tiers = buildSetlists(pool);
    check(tiers.size() == 8, "a big library makes eight venues");
    check(tiers.front().venue == 0 && tiers.back().venue == 7, "venues run from the garage to the stadium");
    for (const auto &t : tiers) {
        check(t.songs.size() == 5, "each venue has four songs and an encore");
        for (const auto &g : t.songs)
            check((g.rating < 0 ? 3.5 : g.rating) <= (t.songs.back().rating < 0 ? 3.5 : t.songs.back().rating),
                  "the encore is the hardest song of its venue");
    }
    check((tiers.front().songs.front().rating) <= (tiers.back().songs.front().rating), "the career gets harder");
    check(buildSetlists({}).empty(), "no songs, no career");
    check(buildSetlists({pool[0], pool[1], pool[2]}).size() == 1, "a tiny library still makes one venue");

    Career c;
    c.active = true, c.band = "Test Band", c.difficulty = 3, c.tiers = tiers;
    Scores scores;
    auto clear = [&](const Gig &g, int stars) { scores.submit(g.folder, "Guitar", 3, {1000, stars, 90, false}); };
    check(c.tierOpen(scores, 0) && !c.tierOpen(scores, 1), "only the first venue starts open");
    check(!c.encoreOpen(scores, 0), "the encore starts locked");
    clear(tiers[0].songs[0], 3);
    clear(tiers[0].songs[1], 4);
    check(!c.encoreOpen(scores, 0), "two songs are not enough for the encore");
    scores.submit(tiers[0].songs[2].folder, "Guitar", 2, {1000, 5, 99, true});
    check(!c.encoreOpen(scores, 0), "a clear at another difficulty does not count");
    clear(tiers[0].songs[2], 5);
    check(c.encoreOpen(scores, 0), "three songs open the encore");
    check(!c.tierOpen(scores, 1), "the next venue waits for the encore");
    clear(tiers[0].songs[4], 3);
    check(c.tierOpen(scores, 1) && !c.tierOpen(scores, 2), "the encore opens the next venue");
    check(c.tierStars(scores, 0) == 15 && c.totalStars(scores) == 15, "stars add up");

    check(c.award(tiers[0].songs[0], 0, 3) == 3 * Career::rate(0), "new stars pay");
    check(c.award(tiers[0].songs[0], 0, 3) == 0, "the same stars never pay twice");
    check(c.award(tiers[0].songs[0], 0, 5) == 2 * Career::rate(0), "only the extra stars pay");
    check(Career::rate(7) > Career::rate(0), "bigger venues pay more");
    const int cash = c.cash;

    c.owned.insert("guitar-gold");
    c.guitar = "guitar-gold", c.pyro = true;
    check(c.owns("guitar-teal") && c.owns("guitar-gold") && !c.owns("jacket-silver"), "free items are always owned");
    const fs::path file = root / "career.cfg";
    c.save(file);
    Career back;
    back.load(file);
    check(back.active && back.band == "Test Band" && back.difficulty == 3, "save keeps the band");
    check(back.cash == cash && back.guitar == "guitar-gold" && back.pyro && back.owns("guitar-gold"), "save keeps the shop");
    check(back.tiers.size() == tiers.size() && back.tiers[3].songs[2].folder == tiers[3].songs[2].folder,
          "save keeps the setlists");
    check(back.award(tiers[0].songs[0], 0, 5) == 0, "save keeps what was paid");
    Career none;
    none.load(root / "missing.cfg");
    check(!none.active, "no file, no career");

    fs::remove_all(root);
    if (failures)
        return EXIT_FAILURE;
    std::cout << "career: all checks passed\n";
    return EXIT_SUCCESS;
}
