// dmusic.h - what the DirectMusic shim (dmusic.cpp) offers beyond its COM
// classes: its switch, the General MIDI bank it found, and test seams.
#pragma once
#include <stddef.h>
#include <string>

// Whether CoCreateInstance makes DirectMusic's classes: game.toml [runtime]
// dmusic, overridden by RECOMP_DMUSIC=0/1, overridden by the test seam.
bool dmusic_enabled();
// -1 goes back to the game's setting; applies at the next dmusic_register.
void dmusic_enable_for_test(int on);
// The host file a band's gm.dls/gs.dls comes from when the game's own folders
// have none: RECOMP_DMUSIC_GM (a DLS collection or a SoundFont 2 bank; naming
// a file that is not there means none), else the system's Roland GS
// collection (macOS), else the kit's General MIDI SoundFont. Empty when there
// is none.
std::string dmusic_gm_bank_path();
// Performances currently rendering into the host's mix.
size_t dmusic_engine_count();
