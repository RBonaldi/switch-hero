#include "career.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace fret::career {
namespace {
std::string lower(std::string s) {
    for (auto &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string &s) {
    const auto a = s.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    if (a == std::string::npos)
        return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
// Tabs and line breaks would break the one-record-per-line file.
std::string field(std::string s) {
    std::replace_if(s.begin(), s.end(), [](char c) { return c == '\t' || c == '\n' || c == '\r'; }, ' ');
    return s;
}
std::vector<std::string> split(const std::string &line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    for (std::string p; std::getline(in, p, '\t');)
        out.push_back(p);
    return out;
}
} // namespace

const std::vector<Venue> &venues() {
    static const std::vector<Venue> v = {
        {"Bento's Garage", "Three amps, one extension cord and a neighbour banging on the wall.", 12},
        {"The Loud Tavern", "First paid gig: free fries and a jar for tips.", 80},
        {"Campus Block Party", "The whole faculty showed up. Half of them came for the free juice.", 400},
        {"Neon Club", "Velvet rope, smoke machine, and a promoter watching from the bar.", 900},
        {"The Underground", "Sweat drips off the ceiling. The crowd knows every word.", 1500},
        {"Sunset Festival", "Open air, golden hour, fifteen thousand people singing along.", 15000},
        {"Metropolis Arena", "Your name in lights. Your face on the big screens.", 30000},
        {"Legends Stadium", "Sold out in four minutes. This is the one they will talk about.", 80000},
        {"The Inferno", "Under the stadium, a door that was never on the map. It is very warm in there.", 666},
        {"Harbour Barge", "A stage bolted to a cargo barge. The whole bay listens from their boats.", 20000},
        {"Glacier Festival", "Walls of ice, an aurora overhead, and a bass line that makes the glacier hum.", 40000},
        {"Orbital Arena", "The first concert in orbit. In space nobody hears you scream, but they hear you shred.", 120000},
    };
    return v;
}

const std::vector<Item> &shop() {
    static const std::vector<Item> items = {
        {"guitar-teal", "Teal Offset", Item::Guitar, 0, 0x1f9e99},
        {"guitar-sunburst", "Sunburst", Item::Guitar, 400, 0xc8641e},
        {"guitar-black", "Matte Black", Item::Guitar, 650, 0x1a1a1e},
        {"guitar-pearl", "Pearl White", Item::Guitar, 900, 0xe8e6de},
        {"guitar-cherry", "Cherry Red", Item::Guitar, 1200, 0xa0141e},
        {"guitar-gold", "Gold Top", Item::Guitar, 2600, 0xd9a93a},
        {"jacket-black", "Black Leather", Item::Jacket, 0, 0x1a1a1c},
        {"jacket-denim", "Denim", Item::Jacket, 350, 0x3c5a8c},
        {"jacket-red", "Red Varsity", Item::Jacket, 750, 0x9a1e24},
        {"jacket-white", "White Studs", Item::Jacket, 1000, 0xdcdcde},
        {"jacket-silver", "Silver Glam", Item::Jacket, 2200, 0xa8acb8},
        {"stage-pyro", "Fire Columns", Item::Stage, 3000, 0},
    };
    return items;
}
const Item *findItem(const std::string &id) {
    for (const auto &i : shop())
        if (id == i.id)
            return &i;
    return nullptr;
}

std::string bandName(unsigned seed) {
    static const char *const names[] = {
        "Blown Fuses",   "Loose Cable",   "Federal Distortion", "Feedback Club",  "The Dead Batteries",
        "Volume Eleven", "Rusty Strings", "The Night Shift",    "Static Saints",  "Garage Kings",
        "Broken Picks",  "Neon Thunder",  "The Overdrives",     "Midnight Amps",  "Second Encore",
    };
    return names[seed % (sizeof names / sizeof *names)];
}

int ratingOf(const fs::path &folder) {
    std::ifstream in(folder / "song.ini");
    for (std::string line; std::getline(in, line);) {
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        if (lower(trim(line.substr(0, eq))) == "diff_guitar") {
            try {
                return std::stoi(trim(line.substr(eq + 1)));
            } catch (...) {
                return -1;
            }
        }
    }
    return -1;
}

double chartWeight(const Track &track) {
    const auto &notes = track.notes;
    if (notes.empty())
        return 0;
    const double length = std::max(30.0, notes.back().time - notes.front().time);
    const double perSecond = double(notes.size()) / length;
    // The densest four seconds: a song is as hard as its hardest run.
    size_t peak = 0;
    for (size_t i = 0, j = 0; i < notes.size(); ++i) {
        while (notes[i].time - notes[j].time > 4)
            ++j;
        peak = std::max(peak, i - j + 1);
    }
    size_t chords = 0;
    for (const auto &n : notes) {
        int frets = 0;
        for (int l = 0; l < 5; ++l)
            frets += (n.mask >> l) & 1;
        chords += frets > 1;
    }
    return perSecond + .5 * (double(peak) / 4) + 1.5 * double(chords) / double(notes.size());
}

std::vector<Tier> buildSetlists(const std::vector<std::pair<fs::path, std::pair<std::string, std::string>>> &songs,
                                int difficulty) {
    struct Candidate {
        Gig gig;
        double weight;
        bool exact; // has a guitar chart at the career's difficulty
    };
    std::vector<Candidate> all, unread;
    for (const auto &[folder, meta] : songs) {
        Candidate c;
        c.gig.folder = folder.filename().string(), c.gig.name = meta.first, c.gig.artist = meta.second;
        try {
            const Song song = loadSong(folder);
            // The chart it will be played on: this difficulty, or the nearest one.
            const Track *best = nullptr;
            for (int step = 0; step < 4 && !best; ++step)
                for (int d : {difficulty - step, difficulty + step})
                    for (const auto &t : song.tracks)
                        if (!best && t.instrument == "Guitar" && t.difficulty == d && !t.notes.empty())
                            best = &t;
            if (!best)
                continue; // no guitar at all
            c.exact = best->difficulty == difficulty;
            c.weight = chartWeight(*best);
            c.gig.rating = int(std::lround(c.weight * 100));
            all.push_back(std::move(c));
        } catch (const std::exception &) {
            // Unreadable here: weigh it by its song.ini rating, unknown in the middle.
            const int r = ratingOf(folder);
            c.exact = false, c.weight = 1 + 1.1 * (r < 0 ? 3.5 : r), c.gig.rating = int(std::lround(c.weight * 100));
            unread.push_back(std::move(c));
        }
    }
    // Songs that would be played on a harder chart than the career's are left
    // out while ten or more songs can be played at the right difficulty.
    const size_t exact = size_t(std::count_if(all.begin(), all.end(), [](const Candidate &c) { return c.exact; }));
    if (exact >= 10)
        all.erase(std::remove_if(all.begin(), all.end(), [](const Candidate &c) { return !c.exact; }), all.end());
    if (all.empty())
        all = std::move(unread);
    std::stable_sort(all.begin(), all.end(), [](const Candidate &a, const Candidate &b) {
        if (a.weight != b.weight)
            return a.weight < b.weight;
        return lower(a.gig.name) < lower(b.gig.name);
    });
    std::vector<Tier> tiers;
    if (all.empty())
        return tiers;
    // Five songs a venue (four and an encore) when the library allows it,
    // at least two otherwise, never more than eight venues.
    const size_t perTier = all.size() >= 40 ? 5 : std::max<size_t>(2, std::min<size_t>(5, all.size() / 8 + 2));
    const size_t extra = size_t(std::clamp(difficulty, 0, 3));
    const size_t count = std::min<size_t>(8 + extra, std::max<size_t>(1, all.size() / perTier));
    // With more songs than places, spread the picks evenly over the whole range.
    const size_t slots = count * perTier;
    for (size_t t = 0; t < count; ++t) {
        Tier tier;
        // The first eight run from the garage to the stadium; any more go past it.
        const size_t base = std::min<size_t>(count, 8);
        tier.venue = t >= 8 ? extraVenues[t - 8]
                   : base == 1 ? 0
                               : int(std::lround(double(t) * 7 / double(base - 1)));
        for (size_t i = 0; i < perTier; ++i) {
            const size_t slot = t * perTier + i;
            const size_t pick = slots >= all.size() ? slot : slot * all.size() / slots;
            if (pick < all.size())
                tier.songs.push_back(all[pick].gig);
        }
        // Lightest first, and the heaviest closes the venue as its encore.
        std::stable_sort(tier.songs.begin(), tier.songs.end(), [](const Gig &a, const Gig &b) { return a.rating < b.rating; });
        if (!tier.songs.empty())
            tiers.push_back(std::move(tier));
    }
    assignDuels(tiers);
    return tiers;
}

void assignDuels(std::vector<Tier> &tiers) {
    // Rivals by venue: the Neon Club, the Sunset Festival, the stadium, and
    // the bosses past it. Lived (3) is placed by addFinalBoss instead.
    for (int r : {0, 1, 2, 4, 5, 6})
        for (auto &t : tiers)
            if (t.venue == hostVenue(r) && t.rival < 0 && !t.songs.empty()) {
                t.rival = r, t.duel = t.songs.back();
                break;
            }
}

void Career::load(const fs::path &path) {
    *this = Career{};
    std::ifstream in(path, std::ios::binary);
    std::string line;
    if (!std::getline(in, line))
        return;
    line = trim(line);
    if (line != "switch-hero career 1" && line != "switch-hero career 2" && line != "switch-hero career 3")
        return;
    // Version 1 was ordered by song.ini ratings; version 2 predates the venues
    // past the stadium. Both are dealt again, keeping everything else.
    staleFrom = line == "switch-hero career 1" ? 1 : line == "switch-hero career 2" ? 2 : 0;
    while (std::getline(in, line)) {
        const auto parts = split(line);
        if (parts.empty())
            continue;
        const std::string &k = parts[0];
        try {
            if (k == "band" && parts.size() > 1)
                band = parts[1];
            else if (k == "mode" && parts.size() > 1)
                bandMode = parts[1] == "band";
            else if (k == "bandstars" && parts.size() > 2)
                bandStars[parts[1]] = std::clamp(std::stoi(parts[2]), 0, 6);
            else if (k == "difficulty" && parts.size() > 1)
                difficulty = std::clamp(std::stoi(parts[1]), 0, 3);
            else if (k == "cash" && parts.size() > 1)
                cash = std::max(0, std::stoi(parts[1]));
            else if (k == "guitar" && parts.size() > 1 && findItem(parts[1]))
                guitar = parts[1];
            else if (k == "jacket" && parts.size() > 1 && findItem(parts[1]))
                jacket = parts[1];
            else if (k == "pyro" && parts.size() > 1)
                pyro = parts[1] == "1";
            else if (k == "owned" && parts.size() > 1 && findItem(parts[1]))
                owned.insert(parts[1]);
            else if (k == "beaten" && parts.size() > 1)
                beaten.insert(std::stoi(parts[1]));
            else if (k == "duel" && parts.size() > 5 && !tiers.empty()) {
                tiers.back().rival = std::clamp(std::stoi(parts[1]), 0, 6);
                tiers.back().duel = {parts[2], parts[3], parts[4], std::stoi(parts[5])};
            }
            else if (k == "paid" && parts.size() > 2)
                paid[parts[1]] = std::stoi(parts[2]);
            else if (k == "tier" && parts.size() > 1) {
                Tier t;
                t.venue = std::clamp(std::stoi(parts[1]), 0, extraVenues[2]);
                tiers.push_back(t);
            } else if (k == "gig" && parts.size() > 4 && !tiers.empty()) {
                Gig g;
                g.folder = parts[1], g.name = parts[2], g.artist = parts[3], g.rating = std::stoi(parts[4]);
                tiers.back().songs.push_back(g);
            }
        } catch (const std::exception &) {
            // A damaged line is skipped; the rest of the career still loads.
        }
    }
    active = !tiers.empty();
    // A version 2 career on Easy has no venues to gain: only harder ones are dealt again.
    if (staleFrom == 2 && difficulty == 0)
        staleFrom = 0;
    stale = staleFrom != 0;
    bool anyDuel = false;
    for (const auto &t : tiers)
        anyDuel = anyDuel || t.rival >= 0;
    if (!anyDuel) // a career from before duels existed
        assignDuels(tiers);
}

void Career::save(const fs::path &path) const {
    fs::path temp = path;
    temp += ".part";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("Cannot save the career: " + path.string());
        out << "switch-hero career 3\n";
        if (bandMode)
            out << "mode\tband\n";
        for (const auto &[folder, stars] : bandStars)
            out << "bandstars\t" << field(folder) << '\t' << stars << '\n';
        out << "band\t" << field(band) << "\ndifficulty\t" << difficulty << "\ncash\t" << cash << "\nguitar\t" << guitar
            << "\njacket\t" << jacket << "\npyro\t" << (pyro ? 1 : 0) << '\n';
        for (const auto &id : owned)
            out << "owned\t" << id << '\n';
        for (int r : beaten)
            out << "beaten\t" << r << '\n';
        for (const auto &[folder, stars] : paid)
            out << "paid\t" << field(folder) << '\t' << stars << '\n';
        for (const auto &t : tiers) {
            out << "tier\t" << t.venue << '\n';
            for (const auto &g : t.songs)
                out << "gig\t" << field(g.folder) << '\t' << field(g.name) << '\t' << field(g.artist) << '\t' << g.rating
                    << '\n';
            if (t.rival >= 0)
                out << "duel\t" << t.rival << '\t' << field(t.duel.folder) << '\t' << field(t.duel.name) << '\t'
                    << field(t.duel.artist) << '\t' << t.duel.rating << '\n';
        }
        if (!out)
            throw std::runtime_error("Cannot save the career: " + path.string());
    }
    std::error_code ec;
    fs::remove(path, ec);
    fs::rename(temp, path);
}

int Career::stars(const Scores &scores, const Gig &gig) const {
    if (bandMode) {
        const auto it = bandStars.find(gig.folder);
        return it == bandStars.end() ? 0 : std::max(1, it->second);
    }
    const Record *r = scores.find(gig.folder, "Guitar", difficulty);
    return r ? std::max(1, r->stars) : 0;
}
bool Career::cleared(const Scores &scores, const Gig &gig) const { return stars(scores, gig) > 0; }
bool Career::encoreOpen(const Scores &scores, size_t tier) const {
    if (tier >= tiers.size() || !tierOpen(scores, tier))
        return false;
    const auto &songs = tiers[tier].songs;
    const size_t main = songs.size() > 1 ? songs.size() - 1 : songs.size();
    size_t done = 0;
    for (size_t i = 0; i < main; ++i)
        done += cleared(scores, songs[i]);
    return done >= std::min<size_t>(3, main);
}
bool Career::tierOpen(const Scores &scores, size_t tier) const {
    if (tier == 0)
        return !tiers.empty();
    if (tier >= tiers.size())
        return false;
    const auto &before = tiers[tier - 1];
    const auto &prev = before.songs;
    return !prev.empty() && tierOpen(scores, tier - 1) && cleared(scores, prev.back()) &&
           (before.rival < 0 || beaten.count(before.rival));
}
bool Career::duelOpen(const Scores &scores, size_t tier) const {
    if (tier < tiers.size() && tiers[tier].rival >= 0 && tiers[tier].songs.empty())
        return tierOpen(scores, tier); // the Inferno: nothing to play first
    return tier < tiers.size() && tiers[tier].rival >= 0 && tierOpen(scores, tier) && !tiers[tier].songs.empty() &&
           cleared(scores, tiers[tier].songs.back());
}
int Career::tierStars(const Scores &scores, size_t tier) const {
    int sum = 0;
    if (tier < tiers.size())
        for (const auto &g : tiers[tier].songs)
            sum += stars(scores, g);
    return sum;
}
int Career::totalStars(const Scores &scores) const {
    int sum = 0;
    for (size_t t = 0; t < tiers.size(); ++t)
        sum += tierStars(scores, t);
    return sum;
}
int Career::award(const Gig &gig, size_t tier, int earned) {
    int &already = paid[gig.folder];
    if (earned <= already)
        return 0;
    const int cashIn = (earned - already) * rate(tier);
    already = earned;
    cash += cashIn;
    return cashIn;
}
void Career::recordBand(const Gig &gig, int earned) {
    int &best = bandStars[gig.folder];
    best = std::max(best, earned);
}
int hostVenue(int rival) {
    switch (rival) {
    case 0: return 3;
    case 1: return 5;
    case 2: return 7;
    case 3: return infernoVenue;
    case 4: return extraVenues[0];
    case 5: return extraVenues[1];
    case 6: return extraVenues[2];
    default: return -1;
    }
}
int Career::duelPrize(int rival) {
    // By the order they are met: the club, festival and stadium, the three
    // bosses past it, and Lived at the very end.
    static const int prize[7] = {1500, 3000, 4500, 12000, 6000, 7500, 9000};
    return rival >= 0 && rival < 7 ? prize[rival] : 0;
}
void addFinalBoss(std::vector<Tier> &tiers, const Gig &song) {
    Tier t;
    t.venue = infernoVenue;
    t.rival = 3;
    t.duel = song;
    tiers.push_back(t);
}
int bandAverage(const std::vector<int> &stars) {
    if (stars.empty())
        return 0;
    int sum = 0;
    for (int s : stars)
        sum += s;
    return sum / int(stars.size());
}
bool Career::owns(const std::string &id) const {
    const Item *i = findItem(id);
    return i && (i->price == 0 || owned.count(id));
}
} // namespace fret::career
