// code_dir.h - the translated code a player host loads instead of linking it.
//
// The player host (CMake target recomp_player, host/README.md "The player
// host") carries no generated code. A game's first run translates and
// compiles the player's own copy into freestanding bundles
// (tools/recomp/bundle.py) and writes them into a code directory:
//
//   <dir>/build.json     {"load_order": ["a.bundle", ..., "main.bundle"], ...}
//   <dir>/<bundle>       each registers its module from a constructor
//   <dir>/symbols.json   the image's translation index (the mods' symbol
//                        table; runtime/layout.cpp finds it through
//                        RECOMP_CODE_DIR or RECOMP_SYMBOLS)
//
// RECOMP_CODE_DIR names the directory. The bundles are dlopen()ed in
// build.json's order, and each must load and register at least one module:
// a host that cannot run the game says why instead of failing at its first
// guest call. Only build.json's "load_order" is read; whatever else the game's
// tools record there is theirs.
#pragma once
#include <string>
#include <vector>

struct CodeDirLoad {
    std::vector<std::string> loaded; // the bundles' paths, in load order
    unsigned modules = 0;            // modules they registered
    unsigned functions = 0;          // translated functions in those modules
};

// Reads <dir>/build.json and dlopen()s every bundle it names. False, with a
// message for a person in *error, when build.json is missing or malformed, a
// name is not a plain file name in `dir`, a bundle is missing or does not
// load, or one registered no translated code. Bundles loaded before the
// failure stay loaded.
bool code_dir_load(const std::string &dir, std::string *error, CodeDirLoad *out = nullptr);

// The load order build.json names, without loading anything (the same checks
// on the names).
bool code_dir_read_order(const std::string &dir, std::vector<std::string> *order,
                         std::string *error);

// BootOptions::load_code for a player host: code_dir_load(RECOMP_CODE_DIR)
// when it is set, nothing otherwise (RECOMP_EXTRA_CODE may still name code,
// and boot refuses an entry point nothing translated).
bool code_dir_load_env(std::string *error);
