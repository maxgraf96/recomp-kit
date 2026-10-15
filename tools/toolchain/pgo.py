#!/usr/bin/env python3
"""Profile-guided optimisation of the bundled toolchain, trained on no game.

    tools/toolchain/pgo.py --clang BUILD/bin/clang --lld BUILD/bin/ld64.lld \\
        --work DIR --profiles BUILD/profiles --out pgo.profdata

PGO makes clang about a quarter faster on generated code (docs in
tools/toolchain/README.md), so the first run on a player's Mac is too. The
profile has to come from compiling something, and that something must be
public and reproducible: it is the translator's own synthetic instruction
vocabulary, the cases the Unicorn differential suite runs
(tools/recomp/tests/test_translate_insns.py), translated by translate.py.

Real translations are files of about 1 MB holding functions from a few
hundred bytes to over 100 KB of C. The corpus is made the same shape from a
seeded generator: each function is a run of case bodies (their labels and
returns renamed apart), sized from a log-normal like a translation's, plus
the multi-entry cases as they are and a table of every function. Each file is
compiled with bundle.py's own flags by the instrumented clang, and the objects
are linked into a bundle by its ld64.lld, so both halves of the toolchain are
trained; the raw profiles are merged with the host compiler's llvm-profdata
(the instrumented build's runtime is the host compiler's).
"""

import argparse
import concurrent.futures
import math
import os
from pathlib import Path
import random
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
KIT = HERE.parents[1]
sys.path.insert(0, str(KIT / "tools" / "recomp"))
sys.path.insert(1, str(KIT / "tools" / "recomp" / "tests"))
import bundle  # noqa: E402

MIN_MACOS = "13.0"                     # bundle.py's --min-macos default

DEFINES = ["GUEST_IMAGE_BASE=0x00400000u", "GUEST_HEAP_BASE=0x01000000u", "GUEST_SIZE=0x10000000u"]
HEADER = '/* PGO training corpus: tools/toolchain/pgo.py, no game */\n#include "x86.h"\n' \
         '#include "intrinsics.h"\n#include "seh.h"\n\n'
SINGLE = re.compile(r"\Avoid (fn_[0-9a-f]+)\(X86 \*c\) \{\n(.*)\}\s*\Z", re.S)
NAME = re.compile(r"\b((?:fn|body)_[0-9a-f]+)\b")
LABEL = re.compile(r"\bL_([0-9a-f]+)\b")


def translated_cases():
    """[C text] of every synthetic case the translator accepts."""
    import translate as T
    from test_translate_insns import CASES, translate_case
    texts = []
    for case in CASES:
        try:
            texts.append(translate_case(case))
        except T.TranslateError:
            pass
    return texts


def corpus(out, chunks=32, chunk_bytes=1 << 20, seed=20261015):
    """Write chunk_NNN.c files and table.c under `out`; return their paths."""
    rng = random.Random(seed)
    texts = translated_cases()
    bodies = [m.group(2) for m in map(SINGLE.match, texts) if m and "body_" not in m.group(2)]
    others = [t for t in texts if not SINGLE.match(t) or "body_" in t]
    if len(bodies) < 20:
        raise SystemExit("pgo: only %d single-entry cases translate; the corpus needs the vocabulary"
                         % len(bodies))
    mean_body = sum(map(len, bodies)) / len(bodies)
    out.mkdir(parents=True, exist_ok=True)
    names, paths = [], []
    for k in range(chunks):
        parts, size, f = [HEADER], 0, 0
        while size < chunk_bytes:
            # Function sizes like a translation's: median ~1.5 KB, a long tail past 100 KB.
            target = min(200000.0, max(200.0, rng.lognormvariate(math.log(1500), 1.5)))
            picks = [rng.choice(bodies) for _ in range(max(1, round(target / mean_body)))]
            name = "corpus_%03d_%04d" % (k, f)
            text = ["void %s(X86 *c) {\n" % name]
            for i, body in enumerate(picks):
                tag = "_%d" % i
                body = LABEL.sub(lambda m: "L_%s%s" % (m.group(1), tag), body)
                body = body.replace("return;", "goto X%s;" % tag)
                text.append("{\n%s}\nX%s: ;\n" % (body, tag))
            text.append("}\n\n")
            parts.append("".join(text))
            names.append(name)
            size += sum(map(len, text))
            f += 1
        for text in others:
            renamed = NAME.sub(lambda m: "%s_c%03d" % (m.group(1), k), text)
            parts.append(renamed + "\n")
            names += ["%s_c%03d" % (n, k) for n in re.findall(r"^void (fn_[0-9a-f]+)\(X86 \*c\)", text, re.M)]
        path = out / ("chunk_%03d.c" % k)
        path.write_text("".join(parts))
        paths.append(path)
    table = [HEADER] + ["void %s(X86 *c);\n" % n for n in names]
    table.append("void (*const corpus_table[%d])(X86 *) = {\n" % len(names))
    table += ["    %s,\n" % n for n in names]
    table.append("};\nconst unsigned corpus_count = %du;\n" % len(names))
    path = out / "table.c"
    path.write_text("".join(table))
    paths.append(path)
    return paths


def train(clang, lld, sources, objdir, jobs):
    """Compile `sources` as bundle.py would, then link them into one bundle."""
    objdir.mkdir(parents=True, exist_ok=True)
    flags = bundle.compile_flags(str(clang), sources[0].parent, sources[0].parent, DEFINES, MIN_MACOS)

    def one(source):
        obj = objdir / (source.stem + ".o")
        subprocess.run([str(clang)] + flags + ["-c", str(source), "-o", str(obj)], check=True)
        return obj

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        objects = list(pool.map(one, sources))
    target = objdir / "corpus.bundle"
    subprocess.run([str(lld), "-arch", "arm64", "-platform_version", "macos", MIN_MACOS, MIN_MACOS,
                    "-bundle", "-undefined", "dynamic_lookup", "-o", str(target)] + [str(o) for o in objects],
                   check=True)
    return target


def merge(profiles, out, profdata=None):
    """Merge the raw profiles into one indexed profile with the host's llvm-profdata."""
    raw = sorted(Path(profiles).glob("*.profraw"))
    if not raw:
        raise SystemExit("pgo: no .profraw files in %s; was the toolchain instrumented?" % profiles)
    tool = profdata or subprocess.check_output(["xcrun", "-f", "llvm-profdata"], text=True).strip()
    subprocess.run([tool, "merge", "-output=%s" % out] + [str(p) for p in raw], check=True)
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--clang", type=Path, required=True, help="the instrumented clang")
    parser.add_argument("--lld", type=Path, required=True, help="the instrumented ld64.lld")
    parser.add_argument("--work", type=Path, required=True, help="corpus and objects")
    parser.add_argument("--profiles", type=Path, required=True, help="where the instrumented build writes .profraw")
    parser.add_argument("--out", type=Path, required=True, help="the merged .profdata")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 4))
    parser.add_argument("--chunks", type=int, default=32)
    args = parser.parse_args(argv)
    sources = corpus(args.work / "corpus", args.chunks)
    train(args.clang, args.lld, sources, args.work / "objects", args.jobs)
    merge(args.profiles, args.out)
    print("pgo: %s from %d files" % (args.out, len(sources)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
