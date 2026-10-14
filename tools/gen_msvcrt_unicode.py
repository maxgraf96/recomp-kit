#!/usr/bin/env python3
"""Generate runtime/msvcrt_unicode.h: character tables for the C runtime HLE.

The tables come from the Unicode Character Database as Python's unicodedata
module ships it (its Unicode 3.2 snapshot, ucd_3_2_0), not from any Windows
file. Windows' msvcrt answers from data of that age; the rules below were
measured against msvcrt.dll (tests/crt in the game repository):
  - kCtypeRuns, the wide classification (iswctype): below 0x100 msvcrt's
    Latin-1 table, in which the ordinals and the micro sign are punctuation;
    above it the C1 type of each code point's Unicode 3.2 category, where a
    symbol or a format character has no class at all (the byte-order mark is
    a blank control), titlecase is upper and lower, and a letter number is
    alphabetic;
  - kAnsiBits, the narrow table of code page 1252 under a real locale: the
    3.2 categories of the characters the bytes stand for, symbols being
    punctuation there;
  - kUpperRuns/kLowerRuns, simple case pairs that existed in Unicode 3.2 and
    map back to where they came from (so no dotless i, long s, final sigma or
    titlecase digraph is changed), as runs;
  - code page 1252 to UTF-16 for 0x80..0x9f (the rest is Latin-1).

With --measured FILE (code point, iswctype bits under "C" and under a real
locale, towupper and towlower under a real locale, hex, one per line; the game
repository's tests/crt/wide_table.py writes it from vectors run against
msvcrt.dll), the measured answers replace the rules for the code points they
cover. They are facts observed by calling the functions.

Run: python3 tools/gen_msvcrt_unicode.py [--measured FILE] > runtime/msvcrt_unicode.h"""

import argparse
import codecs
import sys
import unicodedata

UPPER, LOWER, DIGIT, SPACE, PUNCT, CONTROL, BLANK, HEX, ALPHA = (
    0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80, 0x100)
HEX_DIGITS = set(range(0x30, 0x3A)) | set(range(0x41, 0x47)) | set(range(0x61, 0x67)) | \
    set(range(0xFF10, 0xFF1A)) | set(range(0xFF21, 0xFF27)) | set(range(0xFF41, 0xFF47))
# Latin-1 superscript digits count as digits in Microsoft's C1 data.
SUPERSCRIPT_DIGITS = {0xB2, 0xB3, 0xB9}


OLD = unicodedata.ucd_3_2_0


def category(cp):
    return OLD.category(chr(cp))


def c1_bits(cp, symbols_are_punct):
    cat = category(cp)
    bits = 0
    if cat == "Lu":
        bits = UPPER | ALPHA
    elif cat == "Ll":
        bits = LOWER | ALPHA
    elif cat == "Lt":
        bits = UPPER | LOWER | ALPHA
    elif cat in ("Lm", "Lo", "Nl"):
        bits = ALPHA
    elif cat == "Nd":
        bits = DIGIT
    elif cat == "Zs":
        bits = SPACE | BLANK
    elif cat in ("Zl", "Zp"):
        bits = SPACE
    elif cat == "Cc":
        bits = CONTROL
        if cp in (0x09, 0x0A, 0x0B, 0x0C, 0x0D):
            bits |= SPACE
        if cp == 0x09:
            bits |= BLANK
    elif cat[0] == "P" or cat in ("No", "Mn", "Mc", "Me"):
        bits = PUNCT
    elif cat[0] == "S":
        bits = PUNCT if symbols_are_punct else 0
    if cp == 0xFEFF:
        bits = CONTROL | BLANK
    if cp in SUPERSCRIPT_DIGITS:
        bits |= DIGIT
    if cp in HEX_DIGITS:
        bits |= HEX
    return bits


MEASURED_BITS = {}
MEASURED_UPPER = {}
MEASURED_LOWER = {}


def load_measured(path):
    with open(path) as f:
        for line in f:
            cp, c_bits, real_bits, up, lo = line.split()
            cp = int(cp, 16)
            bits = real_bits if real_bits != "-" else c_bits
            if bits != "-":
                MEASURED_BITS[cp] = int(bits, 16)
            if up != "-":
                MEASURED_UPPER[cp] = int(up, 16)
            if lo != "-":
                MEASURED_LOWER[cp] = int(lo, 16)


def ctype_bits(cp):
    """iswctype's classes."""
    if cp in MEASURED_BITS:
        return MEASURED_BITS[cp]
    if cp < 0x100:
        if cp in (0xAA, 0xB5, 0xBA):
            return PUNCT
        return c1_bits(cp, True)
    return c1_bits(cp, False)


def ansi_bits(b):
    """The narrow classes of a code page 1252 byte under a real locale."""
    try:
        cp = ord(codecs.decode(bytes([b]), "cp1252"))
    except UnicodeDecodeError:
        cp = b
    return c1_bits(cp, True)


def case_runs(fn, back, measured):
    m = {}
    for cp in range(0x10000):
        # Below 256 towupper/towlower answer through the narrow code page
        # tables, so a measurement there says nothing about Unicode pairs.
        if cp in measured and cp >= 0x100:
            if measured[cp] != cp:
                m[cp] = measured[cp]
            continue
        if 0xD800 <= cp < 0xE000 or category(cp) in ("Cn", "Lt"):
            continue
        r = fn(chr(cp))
        if len(r) != 1 or ord(r) == cp or ord(r) >= 0x10000 or category(ord(r)) == "Cn":
            continue
        if back(r) != chr(cp):
            continue  # not a pair: dotless i, long s, final sigma
        m[cp] = ord(r)
    keys = sorted(m)
    out, i = [], 0
    while i < len(keys):
        k, d = keys[i], m[keys[i]] - keys[i]
        for stride in (1, 2):
            j = i
            while j + 1 < len(keys) and keys[j + 1] == keys[j] + stride and m[keys[j + 1]] - keys[j + 1] == d:
                j += 1
            if j > i:
                break
        out.append((k, keys[j], d, stride if j > i else 1))
        i = j + 1
    return out


def main():
    w = sys.stdout.write
    w("// msvcrt_unicode.h - generated by tools/gen_msvcrt_unicode.py from the Unicode\n")
    w("// Character Database %s (classes) and %s (case pairs), with %d code points'\n"
      % (OLD.unidata_version, unicodedata.unidata_version, len(MEASURED_BITS)))
    w("// classes and case measured on msvcrt.dll. Do not edit by hand.\n")
    w("#pragma once\n#include <stdint.h>\n\n// clang-format off\nnamespace crt {\n\n")
    w("struct CaseRun {\n    uint16_t first, last;\n    int32_t delta;\n    uint8_t stride;\n};\n\n")
    for name, fn, back, measured in (("kUpperRuns", str.upper, str.lower, MEASURED_UPPER),
                                     ("kLowerRuns", str.lower, str.upper, MEASURED_LOWER)):
        runs = case_runs(fn, back, measured)
        w("static const CaseRun %s[] = {\n" % name)
        for first, last, delta, stride in runs:
            w("    {0x%04x, 0x%04x, %d, %d},\n" % (first, last, delta, stride))
        w("};\n\n")
    w("struct CtypeRun {\n    uint16_t first;\n    uint16_t bits;\n};\n\n")
    w("// iswctype's classes. Each run covers [first, next run's first).\n")
    w("static const CtypeRun kCtypeRuns[] = {\n")
    prev = None
    line = []
    for cp in range(0x10000):
        if cp in MEASURED_BITS:
            b = MEASURED_BITS[cp]
        else:
            b = 0 if 0xD800 <= cp < 0xE000 or category(cp) in ("Cn", "Co", "Cs") else ctype_bits(cp)
        if b != prev:
            line.append("{0x%04x, 0x%03x}" % (cp, b))
            prev = b
    for i in range(0, len(line), 5):
        w("    " + ", ".join(line[i:i + 5]) + ",\n")
    w("};\n\n")
    w("// The narrow classes of code page 1252's bytes under a real locale.\n")
    w("static const uint16_t kAnsiBits[256] = {\n")
    vals = [ansi_bits(b) for b in range(256)]
    for i in range(0, 256, 8):
        w("    " + ", ".join("0x%03x" % v for v in vals[i:i + 8]) + ",\n")
    w("};\n\n")
    w("// Code page 1252, 0x80..0x9f; undefined bytes map to the C1 control of the same value.\n")
    w("static const uint16_t kCp1252High[32] = {\n")
    vals = []
    for b in range(0x80, 0xA0):
        try:
            vals.append(ord(codecs.decode(bytes([b]), "cp1252")))
        except UnicodeDecodeError:
            vals.append(b)
    for i in range(0, 32, 8):
        w("    " + ", ".join("0x%04x" % v for v in vals[i:i + 8]) + ",\n")
    w("};\n\n} // namespace crt\n// clang-format on\n")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--measured", help="measured wide table (tests/crt/wide_table.py)")
    args = ap.parse_args()
    if args.measured:
        load_measured(args.measured)
    main()
