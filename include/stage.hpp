#pragma once
// The stage behind the highway: an original band (a singer at the mic, the
// player's guitarist and a drummer on the riser), lights, an LED wall and a
// crowd. Everything is drawn procedurally with the look:: primitives and
// reacts to the song and to how the player is doing.
#include <cstdint>

namespace fret::stage {
struct Cues {
    double time = 0;  // song seconds
    double beat = 0;  // beats since the start of the song (fractional)
    int combo = 0, multiplier = 1, hits = 0, misses = 0;
    bool power = false, failed = false;
    double meter = .5;    // rock meter, 0 (booed off) to 1
    double meterRed = .28; // below this the crowd turns
    uint8_t held = 0;     // frets held, bit 0 = green
    // Career: which venue (0 garage .. 7 stadium, -1 the Quickplay club) and
    // what the band bought in the shop.
    int venue = -1;
    unsigned guitarColor = 0x1f9e99, jacketColor = 0x1a1a1c;
    bool pyro = false;
    // Duels: the rival takes the singer's spot (0 Dona Rubi, 1 Viktor Kade, 2 MK-77).
    int rival = -1;
    unsigned rivalColor = 0xffffff;
    double rivalHurtAt = -1e9;   // song time the player's attack landed
    double rivalStrikeAt = -1e9; // song time the rival's attack landed on the player
};
// Draws the whole backdrop, replacing look::wall(). Call before the highway.
void draw(const Cues &cues);
// Forget hit/miss/star power history; call when a song starts or restarts.
void reset();
} // namespace fret::stage
