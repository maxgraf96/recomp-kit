#!/usr/bin/env python3
"""Generate runtime/codepage_tables.h from a Windows capture of the code page
conversions (tools/codepages/capture_win32.c, committed as
runtime/tests/data/codepages_windows.tsv).

Per single-byte code page the header holds:
  - the byte -> UTF-16 table (MultiByteToWideChar, no flags);
  - the MB_USEGLYPHCHARS table where it differs, and the bytes
    MB_ERR_INVALID_CHARS rejects;
  - the MB_COMPOSITE decompositions;
  - the exact UTF-16 -> byte mappings (what WC_NO_BEST_FIT_CHARS keeps) and the
    best-fit ones Windows adds without that flag;
  - the WC_COMPOSITECHECK compositions: a base and a combining mark whose
    canonical composition the code page can spell, exactly or by best fit
    (Python's Unicode database names the compositions, the capture decides
    which ones the code page spells).

  gen_tables.py                 write the header
  gen_tables.py --check         fail when the committed header differs
"""

import argparse
from pathlib import Path
import sys
import unicodedata

ROOT = Path(__file__).resolve().parents[2]
CAPTURE = ROOT / "runtime/tests/data/codepages_windows.tsv"
HEADER = ROOT / "runtime/codepage_tables.h"

MB_ERR_INVALID_CHARS, MB_USEGLYPHCHARS, MB_COMPOSITE = 0x8, 0x4, 0x2
WC_NO_BEST_FIT_CHARS = 0x400


def load(path):
    windows, mb, wc = "", {}, {}
    for line in path.read_text().splitlines():
        f = line.split("\t")
        if f[0] == "# windows":
            windows = f[1]
        elif f[0] == "mb":
            units = [int(u, 16) for u in f[6].split()] if len(f) > 6 and f[6] else []
            mb[(int(f[1]), int(f[2], 16), int(f[3], 16))] = (int(f[4]), f[5], units)
        elif f[0] == "wc":
            data = bytes.fromhex(f[6])
            wc[(int(f[1]), int(f[2], 16), int(f[3], 16))] = (int(f[4]), f[5], data, int(f[7]))
    return windows, mb, wc


def table(cp, mb, wc):
    to_unicode, glyphs, invalid, composite = [], [], [], []
    for b in range(256):
        ret, _, units = mb[(cp, 0, b)]
        assert ret == 1, (cp, b)
        to_unicode.append(units[0])
        glyphs.append(mb[(cp, MB_USEGLYPHCHARS, b)][2][0])
        if mb[(cp, MB_ERR_INVALID_CHARS, b)][0] == 0:
            invalid.append(b)
        ret, _, units = mb[(cp, MB_COMPOSITE, b)]
        if ret == 2:
            composite.append((b, units[0], units[1]))
        else:
            assert ret == 1 and units[0] == to_unicode[b], (cp, b)
    exact, best_fit = {}, {}
    for (c, flags, unit), (ret, _, data, used) in wc.items():
        if c == cp and flags == WC_NO_BEST_FIT_CHARS:
            assert ret == 1 and used == 0, (cp, unit)
            exact[unit] = data[0]
    # The exact mappings are the inverse of the byte table, the lowest byte
    # winning where two bytes give one unit: the runtime derives them.
    inverse = {}
    for b, unit in enumerate(to_unicode):
        inverse.setdefault(unit, b)
    assert exact == inverse, cp
    for (c, flags, unit), (ret, _, data, used) in wc.items():
        if c != cp or flags != 0:
            continue
        assert ret == 1 and used == 0, (cp, unit)
        if unit in exact:
            assert exact[unit] == data[0], (cp, unit)
        else:
            best_fit[unit] = data[0]
    for unit, b in exact.items():
        assert wc.get((cp, 0, unit), (1, "", bytes([b]), 0))[2][0] == b, (cp, unit)
    return {
        "to_unicode": to_unicode,
        "glyphs": glyphs if glyphs != to_unicode else None,
        "invalid": invalid,
        "composite": composite,
        "exact": exact,
        "best_fit": sorted(best_fit.items()),
    }


def compositions(tables):
    """Base + combining mark -> composed character, for every composed
    character some code page spells (exactly or by best fit): Unicode's
    canonical two-part compositions, plus the inverse of each code page's
    measured MB_COMPOSITE decompositions (Windows decomposes O WITH STROKE,
    which Unicode does not)."""
    spelled = set()
    found = {}
    for t in tables.values():
        spelled.update(t["exact"])
        spelled.update(u for u, _ in t["best_fit"])
        for b, base, mark in t["composite"]:
            found[(base, mark)] = t["to_unicode"][b]
    for code in range(0x80, 0x10000):
        if code not in spelled or 0xd800 <= code < 0xe000:
            continue
        ch = chr(code)
        dec = unicodedata.decomposition(ch)
        if not dec or dec.startswith("<"):
            continue
        parts = [int(p, 16) for p in dec.split()]
        if len(parts) != 2 or parts[1] > 0xffff or not unicodedata.combining(chr(parts[1])):
            continue
        if unicodedata.normalize("NFC", chr(parts[0]) + chr(parts[1])) != ch:
            continue  # a composition exclusion
        found.setdefault((parts[0], parts[1]), code)
    return sorted((base, mark, composed) for (base, mark), composed in found.items())


def rows(values, per_line, fmt):
    out = []
    for i in range(0, len(values), per_line):
        out.append("    " + " ".join(fmt(v) + "," for v in values[i:i + per_line]))
    return "\n".join(out)


def render(windows, mb, wc):
    cps = sorted({k[0] for k in mb}, key=lambda c: (c != 1252, c))
    tables = {cp: table(cp, mb, wc) for cp in cps}
    out = [
        "// codepage_tables.h - Windows' single-byte code pages, as MultiByteToWideChar",
        "// and WideCharToMultiByte apply them. Generated by tools/codepages/gen_tables.py",
        f"// from runtime/tests/data/codepages_windows.tsv (Windows {windows}); do not edit.",
        "// clang-format off",
        "#pragma once",
        "#include <stdint.h>",
        "",
        "namespace codepage_tables {",
        "",
        "struct Pair { uint16_t unit; uint8_t byte; };",
        "struct Decomposition { uint8_t byte; uint16_t base, mark; };",
        "struct Composition { uint16_t base, mark, composed; };",
        "// The exact UTF-16 -> byte mappings are the inverse of to_unicode, the",
        "// lowest byte winning; best_fit adds what WC_NO_BEST_FIT_CHARS refuses.",
        "struct Sbcs {",
        "    uint16_t cp;",
        "    const uint16_t *to_unicode; // 256 entries",
        "    const uint16_t *glyphs;     // MB_USEGLYPHCHARS, or null when the same",
        "    const uint8_t *invalid;     // bitmap: MB_ERR_INVALID_CHARS rejects the byte",
        "    const Pair *best_fit; uint16_t best_fit_count; // sorted by unit",
        "    const Decomposition *decompositions; uint16_t decomposition_count; // MB_COMPOSITE",
        "};",
        "",
    ]
    entries = []
    for cp in cps:
        t = tables[cp]
        n = f"k{cp}"
        out.append(f"static const uint16_t {n}ToUnicode[256] = {{")
        out.append(rows(t["to_unicode"], 8, lambda v: f"0x{v:04x}"))
        out.append("};")
        if t["glyphs"]:
            out.append(f"static const uint16_t {n}Glyphs[256] = {{")
            out.append(rows(t["glyphs"], 8, lambda v: f"0x{v:04x}"))
            out.append("};")
        bitmap = [0] * 32
        for b in t["invalid"]:
            bitmap[b >> 3] |= 1 << (b & 7)
        out.append(f"static const uint8_t {n}Invalid[32] = {{")
        out.append(rows(bitmap, 16, lambda v: f"0x{v:02x}"))
        out.append("};")
        if t["best_fit"]:
            out.append(f"static const Pair {n}BestFit[{len(t['best_fit'])}] = {{")
            out.append(rows(t["best_fit"], 6, lambda p: f"{{0x{p[0]:04x}, 0x{p[1]:02x}}}"))
            out.append("};")
        if t["composite"]:
            out.append(f"static const Decomposition {n}Decompositions[{len(t['composite'])}] = {{")
            out.append(rows(t["composite"], 4, lambda d: f"{{0x{d[0]:02x}, 0x{d[1]:04x}, 0x{d[2]:04x}}}"))
            out.append("};")
        out.append("")

        def ref(name, count):
            return (f"{n}{name}, {count}") if count else "nullptr, 0"

        entries.append(
            f"    {{{cp}, {n}ToUnicode, {n + 'Glyphs' if t['glyphs'] else 'nullptr'}, {n}Invalid,\n"
            f"     {ref('BestFit', len(t['best_fit']))}, {ref('Decompositions', len(t['composite']))}}},")
    out.append(f"static const Sbcs kSbcs[{len(entries)}] = {{")
    out.extend(entries)
    out.append("};")
    out.append("")
    comp = compositions(tables)
    out.append("// WC_COMPOSITECHECK: a base and a combining mark the code pages can spell")
    out.append("// composed, sorted by base and mark. The composed character is then converted")
    out.append("// as any other, exactly or by best fit.")
    out.append(f"static const Composition kCompositions[{len(comp)}] = {{")
    out.append(rows(comp, 4, lambda d: f"{{0x{d[0]:04x}, 0x{d[1]:04x}, 0x{d[2]:04x}}}"))
    out.append("};")
    out.append("")
    out.append("} // namespace codepage_tables")
    out.append("// clang-format on")
    return "\n".join(out) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="compare with the committed header")
    parser.add_argument("--capture", type=Path, default=CAPTURE)
    parser.add_argument("--out", type=Path, default=HEADER)
    args = parser.parse_args()
    text = render(*load(args.capture))
    if args.check:
        if not args.out.is_file() or args.out.read_text() != text:
            print(f"{args.out} is not gen_tables.py's output for {args.capture}")
            return 1
        print(f"{args.out} is current")
        return 0
    args.out.write_text(text)
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
