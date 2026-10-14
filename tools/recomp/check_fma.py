#!/usr/bin/env python3
"""Fail when compiled code contains a fused multiply-add.

    .venv/bin/python tools/recomp/check_fma.py OBJECT_OR_ARCHIVE...

x87 rounds every multiply and every add on its own. A compiler that
contracts `a * b + c` into one fused instruction rounds once, so the exact
x87 (runtime/x86.h, runtime/x87.c) and the generated code are built with
-ffp-contract=off; this is the check that it held. It disassembles the
given objects, static archives or shared libraries and reports every
fmadd/fmsub/fnmadd/fnmsub (arm64) or vfmadd/vfmsub/vfnmadd/vfnmsub (x86-64)
with the function it sits in. Exit status 1 if there is one.

tools/recomp/tests/test_x87_fma.py runs it over the x87 helpers and
translated x87 code in tools/test.py. Point it at a game's generated
archive (build/cmake/<preset>/lib/librecomp_gen.a) to check a real build."""

import argparse
import re
import shutil
import subprocess
import sys

FMA = re.compile(r"\b(v?f(?:n?m(?:add|sub))(?:[0-9a-z]*))\b", re.I)
# A disassembly line: address, optional encoding, then the mnemonic.
LINE = re.compile(r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2}\s)*\s*([a-z][a-z0-9.]*)", re.I)
SYMBOL = re.compile(r"^[0-9a-f]+ <(.*)>:$", re.I)


def objdump():
    for name in ("llvm-objdump", "objdump"):
        path = shutil.which(name)
        if path:
            return [path]
    if shutil.which("xcrun"):
        return ["xcrun", "objdump"]
    raise RuntimeError("no objdump on PATH")


def scan(paths):
    """[(path, function, instruction)] for every fused multiply-add."""
    found = []
    for path in paths:
        text = subprocess.run(objdump() + ["-d", "--no-show-raw-insn", path], check=True,
                              capture_output=True, text=True).stdout
        function = "?"
        for line in text.splitlines():
            m = SYMBOL.match(line.strip())
            if m:
                function = m.group(1)
                continue
            m = LINE.match(line)
            if m and FMA.fullmatch(m.group(1)):
                found.append((path, function, line.strip()))
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+")
    a = ap.parse_args()
    found = scan(a.paths)
    for path, function, line in found:
        print("%s: %s: %s" % (path, function, line))
    if found:
        print("%d fused multiply-add instruction(s): build with -ffp-contract=off" % len(found))
        return 1
    print("no fused multiply-add in %d file(s)" % len(a.paths))
    return 0


if __name__ == "__main__":
    sys.exit(main())
