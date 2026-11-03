#pragma once
// Career mode: the band's climb from a garage to a stadium.
//
// Setlists are built from the player's own library, easiest charts first, and
// split into eight venues of four songs plus an encore. Clearing three songs of
// a venue opens its encore; clearing the encore opens the next venue. New stars
// earn the band cash for the shop, which sells looks for the stage. Stars come
// from the same scores.cfg records Quickplay keeps, so a song already beaten in
// Quickplay at the career's difficulty counts.
#include "scores.hpp"
#include "song.hpp"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fret::career {

struct Venue {
    const char *name;  // English; translated for display
    const char *blurb; // one line of story
    int capacity;      // shown on the venue card
};
const std::vector<Venue> &venues(); // always eight, in career order

struct Gig { // one song of a setlist
    std::string folder, name, artist;
    int rating = -1; // song.ini diff_guitar, -1 when unknown
};
struct Tier {
    int venue = 0;
    std::vector<Gig> songs; // the last one is the encore
};

struct Item {
    const char *id, *name; // name in English; translated for display
    enum Kind { Guitar, Jacket, Stage } kind;
    int price;
    unsigned color; // 0xRRGGBB for guitars and jackets
};
const std::vector<Item> &shop();
const Item *findItem(const std::string &id);

struct Career {
    bool active = false;
    std::string band;
    int difficulty = 1; // 0 Easy .. 3 Expert
    std::vector<Tier> tiers;
    int cash = 0;
    std::map<std::string, int> paid; // stars already paid out, per song folder
    std::set<std::string> owned;     // shop item ids
    std::string guitar = "guitar-teal", jacket = "jacket-black";
    bool pyro = false; // fire columns bought and switched on

    void load(const fs::path &path);
    void save(const fs::path &path) const;

    // Stars for one gig at the career's difficulty: 0 when never cleared.
    int stars(const Scores &scores, const Gig &gig) const;
    bool cleared(const Scores &scores, const Gig &gig) const;
    bool tierOpen(const Scores &scores, size_t tier) const;
    bool encoreOpen(const Scores &scores, size_t tier) const;
    int tierStars(const Scores &scores, size_t tier) const;
    int totalStars(const Scores &scores) const;
    // Pays for stars not paid before, at this venue's rate. Returns the cash earned.
    int award(const Gig &gig, size_t tier, int stars);
    static int rate(size_t tier) { return 120 + 90 * int(tier); } // cash per new star
    bool owns(const std::string &id) const;
};

// Setlists from the library: songs sorted by their guitar rating (unknown
// ratings sit in the middle) and song length, dealt into up to eight venues.
std::vector<Tier> buildSetlists(const std::vector<std::pair<fs::path, std::pair<std::string, std::string>>> &songs);
// A guitar difficulty from a folder's song.ini, or -1.
int ratingOf(const fs::path &folder);
// An original band name, picked by `seed`.
std::string bandName(unsigned seed);
} // namespace fret::career
