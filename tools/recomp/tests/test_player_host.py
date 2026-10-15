"""The player host (host/CMakeLists.txt recomp_player, host/README.md "The
player host") carries no translated code: a game gives it away, and the
translation is built from each player's own copy on their machine.

ctest runs this with RECOMP_PLAYER_HOST_BINARY naming the built executable
(inside build/player/<app>.app); without it, as in the portable suite, the
checks are skipped. They read the binary's symbol table (nm), so they need
the developer tools that built it."""
import os
import re
import shutil
import subprocess
import unittest
from pathlib import Path

BINARY = os.environ.get("RECOMP_PLAYER_HOST_BINARY")

# What generated code defines: a function per guest address and its bodies,
# and the image table's own symbols, which the runtime defines weakly
# (runtime/dispatch.cpp, profile.cpp) so a linked table replaces them.
TRANSLATED = re.compile(r"^_?(fn|body)_[0-9a-fA-F]{6,8}\b|^_?[a-z0-9_]+_fn_[0-9a-fA-F]{6,8}\b")
TABLE = ("_recomp_func_addrs", "_recomp_func_count", "_recomp_base_ptrs", "_recomp_raw_ptrs",
         "_recomp_hook_ptrs", "_recomp_hooked", "_recomp_index_of", "_recomp_is_call_return",
         "_recomp_override_count", "_recomp_override_hash", "_recomp_profile_name",
         "_recomp_call", "_recomp_jump", "_recomp_unknown_jump")
# `nm -m`: "<address> (<segment>,<section>) [weak] [private] external <name> [(from <library>)]".
NAME = re.compile(r"\b(?:non-)?external\s+(\S+)")


def symbols(binary):
    """[(name, description)] from `nm -m` (Mach-O: the description says weak or not)."""
    nm = shutil.which("nm") or "/usr/bin/nm"
    out = subprocess.run([nm, "-m", str(binary)], capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        m = NAME.search(line)
        if m:
            rows.append((m.group(1), line))
    return rows


@unittest.skipUnless(BINARY, "RECOMP_PLAYER_HOST_BINARY is not set (ctest sets it)")
class PlayerHost(unittest.TestCase):
    def setUp(self):
        self.binary = Path(BINARY)
        self.assertTrue(self.binary.is_file(), "no player host at %s" % self.binary)

    def test_no_translated_functions(self):
        stray = [name for name, _ in symbols(self.binary) if TRANSLATED.search(name)]
        self.assertEqual(stray[:10], [], "translated functions in the player host (%d)" % len(stray))

    def test_image_table_is_the_runtimes_empty_one(self):
        defined = {name: line for name, line in symbols(self.binary)
                   if name in TABLE and "undefined" not in line}
        self.assertIn("_recomp_call", defined, "the runtime's dispatch is missing")
        strong = sorted(name for name, line in defined.items() if "weak" not in line)
        self.assertEqual(strong, [], "defined strongly: a generated table is linked in")

    def test_bundle_carries_nothing_from_the_build_root(self):
        if self.binary.parent.name != "MacOS":
            self.skipTest("not inside an app bundle")
        resources = self.binary.parent.parent / "Resources"
        self.assertFalse((resources / "symbols.json").exists(), "symbols.json in the player host")
        self.assertFalse((resources / "texture-pack").exists(), "a texture pack in the player host")
        self.assertEqual(self.binary.parents[2].parent.name, "player")

    def test_starts_without_a_translation(self):
        # Answered before SDL, the GPU or the game: the binary loads and links.
        env = {k: v for k, v in os.environ.items() if not k.startswith("RECOMP_")}
        for flag in ("--version", "--probe-layout"):
            result = subprocess.run([str(self.binary), flag], capture_output=True, text=True,
                                    timeout=60, env=env)
            self.assertEqual(result.returncode, 0, "%s: %s" % (flag, result.stderr))
            self.assertTrue(result.stdout.strip(), flag)


if __name__ == "__main__":
    unittest.main()
