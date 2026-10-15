#!/usr/bin/env python3
"""Hold the runtime's GDI text metrics against Windows' for the same requests.

    compare.py WINDOWS.jsonl RUNTIME.jsonl
    compare.py WINDOWS.jsonl --gdi-tests build/recomp/gdi_tests [--cases cases.txt]

Both files are what capture_win32.c (on Windows) and `gdi_tests --capture`
(here) write for tools/gdi_metrics/cases.txt: one line per font request with
its TEXTMETRICA, face name, the GetCharWidth32A widths of 0x20..0xFF and the
GetTextExtentPoint32A extent of every string. The second form runs the
runtime's capture itself.

The tolerances say what a game's layout can live with:

  face name                                exact (case-insensitive)
  height, ascent, descent, both leadings   exact: line spacing and baselines
  average and maximum character width      within 1 pixel
  weight, overhang, digitized aspect,
  character range, pitch and family,
  character set                            exact
  italic, underlined, struck out           the same yes or no
  character widths                         every one within 1 pixel, and at
                                           least 98 % of them exact per font
  string extents                           heights exact; widths within
                                           max(1, 1 %) and at least 90 % of
                                           the strings exact per font

Exit status 0 when every font is within them, 1 otherwise; the report lists
each font that is not and, with -v, every difference.

--only FIELD,... holds just those parts against Windows: TEXTMETRIC field
names (height, ascent, ...), "face", "widths" and "extents". It is how a
capture of faces whose installed copies are another version than Windows'
(their bounding boxes, and so tmMaxCharWidth, differ) still guards the rest.
--skip-chars HEX,... leaves those characters' widths out.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

TM_FIELDS = ["height", "ascent", "descent", "internal", "external", "ave", "max", "weight",
             "overhang", "aspect_x", "aspect_y", "first", "last", "default", "break", "italic",
             "underlined", "struck", "pitch_family", "charset"]
EXACT = {"height", "ascent", "descent", "internal", "external", "weight", "overhang",
         "aspect_x", "aspect_y", "first", "last", "default", "break", "pitch_family", "charset"}
WITHIN_ONE = {"ave", "max"}
TRUTH = {"italic", "underlined", "struck"}


def load(path):
    cases = {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            row = json.loads(line)
            cases[(row["face"], row["height"], row["weight"], row["italic"])] = row
    return cases


def compare_case(win, mac, only=None, skip_chars=()):
    """The differences beyond tolerance, and the ones within it, as text.
    `only`, when given, is the set of parts to compare (see --only)."""
    bad, notes = [], []
    wanted = (lambda part: True) if only is None else (lambda part: part in only)
    if wanted("face") and win["realized"].lower() != mac["realized"].lower():
        bad.append("face %r, Windows %r" % (mac["realized"], win["realized"]))
    for i, name in enumerate(TM_FIELDS):
        if not wanted(name):
            continue
        w, m = win["tm"][i], mac["tm"][i]
        if w == m:
            continue
        if name in TRUTH and bool(w) == bool(m):
            continue
        if name in WITHIN_ONE and abs(w - m) <= 1:
            notes.append("%s %d, Windows %d" % (name, m, w))
            continue
        bad.append("%s %d, Windows %d" % (name, m, w))
    widths = [(w, m) for i, (w, m) in enumerate(zip(win["widths"], mac["widths"]))
              if 0x20 + i not in skip_chars] if wanted("widths") else []
    chars = [0x20 + i for i in range(len(win["widths"])) if 0x20 + i not in skip_chars]
    off = [(chars[i], w, m) for i, (w, m) in enumerate(widths) if w != m]
    far = [d for d in off if abs(d[1] - d[2]) > 1]
    if far:
        bad.append("widths more than 1 pixel off: " +
                   ", ".join("%02x %d/%d" % (c, m, w) for c, w, m in far[:8]))
    if widths and len(off) > 0.02 * len(widths):
        bad.append("%d of %d widths differ" % (len(off), len(widths)))
    elif off:
        notes.append("%d widths differ by 1" % len(off))
    extents = list(zip(win["extents"], mac["extents"])) if wanted("extents") else []
    inexact = 0
    for i, ((wx, wy), (mx, my)) in enumerate(extents):
        if wy != my:
            bad.append("string %d height %d, Windows %d" % (i, my, wy))
        if wx != mx:
            inexact += 1
            if abs(wx - mx) > max(1, wx // 100):
                bad.append("string %d width %d, Windows %d" % (i, mx, wx))
    if extents and inexact > 0.1 * len(extents):
        bad.append("%d of %d string widths differ" % (inexact, len(extents)))
    elif inexact:
        notes.append("%d string widths differ slightly" % inexact)
    return bad, notes


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("windows")
    parser.add_argument("runtime", nargs="?")
    parser.add_argument("--gdi-tests", help="run this gdi_tests binary's capture")
    parser.add_argument("--cases", default=os.path.join(os.path.dirname(__file__), "cases.txt"))
    parser.add_argument("--only", help="compare just these parts (comma-separated)")
    parser.add_argument("--skip-chars", default="", help="hex character codes to leave out")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    only = set(args.only.split(",")) if args.only else None
    skip_chars = {int(c, 16) for c in args.skip_chars.split(",") if c}
    runtime = args.runtime
    if not runtime:
        if not args.gdi_tests:
            parser.error("give RUNTIME.jsonl or --gdi-tests")
        fd, runtime = tempfile.mkstemp(suffix=".jsonl")
        os.close(fd)
        subprocess.run([args.gdi_tests, "--capture", args.cases, runtime], check=True,
                       stdout=subprocess.DEVNULL)
    win, mac = load(args.windows), load(runtime)
    missing = sorted(set(win) - set(mac))
    failed = 0
    for key in sorted(win):
        if key not in mac:
            continue
        bad, notes = compare_case(win[key], mac[key], only, skip_chars)
        label = "%s %d weight %d%s" % (key[0], key[1], key[2], " italic" if key[3] else "")
        if bad:
            failed += 1
            print("FAIL %s: %s" % (label, "; ".join(bad)))
        if args.verbose and notes:
            print("     %s: %s" % (label, "; ".join(notes)))
    for key in missing:
        print("MISSING %s %d weight %d italic %d: not in the runtime capture" % key)
    checked = len(set(win) & set(mac))
    print("%d font requests compared, %d outside the tolerances, %d missing"
          % (checked, failed, len(missing)))
    return 1 if failed or missing else 0


if __name__ == "__main__":
    sys.exit(main())
