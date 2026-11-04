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
// Eight venues in career order, then the Inferno: the band career's last
// stop, where only the final boss plays.
const std::vector<Venue> &venues();
constexpr int infernoVenue = 8;

struct Gig { // one song of a setlist
    std::string folder, name, artist;
    int rating = -1; // how hard the chart is to play (chartWeight x 100), -1 when unknown
};
struct Tier {
    int venue = 0;
    std::vector<Gig> songs; // the last one is the encore
    int rival = -1;         // a duel after the encore at the big venues (duel.hpp), or -1
    Gig duel;               // the song of that duel
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
    // A band career (two to four players): stars come from the band's own
    // record below instead of scores.cfg, and the Inferno waits at the end.
    bool bandMode = false;
    std::map<std::string, int> bandStars; // best band stars per song folder
    std::string band;
    int difficulty = 1; // 0 Easy .. 3 Expert
    std::vector<Tier> tiers;
    int cash = 0;
    std::map<std::string, int> paid; // stars already paid out, per song folder
    std::set<std::string> owned;     // shop item ids
    std::set<int> beaten;            // rivals beaten in a duel
    std::string guitar = "guitar-teal", jacket = "jacket-black";
    bool pyro = false; // fire columns bought and switched on
    bool stale = false; // setlists from before they were ordered by the charts themselves

    void load(const fs::path &path);
    void save(const fs::path &path) const;

    // Stars for one gig at the career's difficulty: 0 when never cleared.
    int stars(const Scores &scores, const Gig &gig) const;
    bool cleared(const Scores &scores, const Gig &gig) const;
    bool tierOpen(const Scores &scores, size_t tier) const;
    bool encoreOpen(const Scores &scores, size_t tier) const;
    // The duel opens with the encore cleared; beating it opens the next venue.
    bool duelOpen(const Scores &scores, size_t tier) const;
    static int duelPrize(int rival) { return 1500 + 1500 * rival; }
    // Keeps the band's best stars for a gig (band careers only).
    void recordBand(const Gig &gig, int stars);
    int tierStars(const Scores &scores, size_t tier) const;
    int totalStars(const Scores &scores) const;
    // Pays for stars not paid before, at this venue's rate. Returns the cash earned.
    int award(const Gig &gig, size_t tier, int stars);
    static int rate(size_t tier) { return 120 + 90 * int(tier); } // cash per new star
    bool owns(const std::string &id) const;
};

// How hard a chart is to play, from the notes themselves rather than the
// charter's own rating, which no two charters agree on: notes per second over
// the song, its densest four seconds, and how many notes are chords.
double chartWeight(const Track &track);
// Setlists from the library, as a ladder: each song is weighed on the guitar
// chart it will actually be played on at `difficulty` and the songs are dealt,
// lightest first, into up to eight venues. Songs without a chart at that
// difficulty are left out while there are enough others (they would be played
// on a harder one). Songs whose chart cannot be read fall back to song.ini.
std::vector<Tier> buildSetlists(const std::vector<std::pair<fs::path, std::pair<std::string, std::string>>> &songs,
                                int difficulty);
// A guitar difficulty from a folder's song.ini, or -1.
int ratingOf(const fs::path &folder);
// Gives the venues that host a duel (the club, the festival, the stadium)
// their rival, fighting over the venue's encore. Old careers get them on load.
void assignDuels(std::vector<Tier> &tiers);
// The band career's final stop: the Inferno, where the final boss (duel.hpp's
// last rival) plays `song`. Appended after the stadium.
void addFinalBoss(std::vector<Tier> &tiers, const Gig &song);
// The band's stars for a song: the average of everyone's, rounded down, so the
// whole band has to play well for a good result.
int bandAverage(const std::vector<int> &stars);
// An original band name, picked by `seed`.
std::string bandName(unsigned seed);
} // namespace fret::career
