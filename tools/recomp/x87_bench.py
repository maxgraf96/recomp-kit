#!/usr/bin/env python3
"""Time the x87 arithmetic paths: the exact x87's double fast path, the
double-only helpers it replaced, the PC=24 shortcut and the SoftFloat path.

    .venv/bin/python tools/recomp/x87_bench.py [--old-rev REV] [--n 200000000]

Two kernels, each making the calls the translator emits:
  chain   FMUL m64, FADD m64 on ST(0), a dependency chain;
  stream  per element FLD m32, FMUL m32, FADD m32, FSTP m32, the shape of a
          game's float maths.
Reported in ns per x87 instruction, best of --repeat runs. The old helpers
come from runtime/x86.h at --old-rev (default: the parent of the commit that
added runtime/x87.c). Built at -O2 with -ffp-contract=off, as the game is.
Timings depend on the machine; publish them with its name."""

import argparse
import os
import platform
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

MAIN = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "x86.h"

uint8_t *g_mem;
uint32_t g_watch_base, g_watch_len;
RecompDirty g_dirty[RECOMP_DIRTY_SLOTS];
uint32_t g_dirty_count;
void recomp_watch_hit(uint32_t a, uint32_t n, uint64_t v) { (void)a; (void)n; (void)v; }
void recomp_null_access(uint32_t a, int w) { (void)a; (void)w; abort(); }

double bench_chain_new(uint16_t, const double *, const double *, uint32_t, uint32_t);
double bench_chain_old(uint16_t, const double *, const double *, uint32_t, uint32_t);
void bench_stream_new(uint16_t, uint32_t, uint32_t, uint32_t, uint32_t);
void bench_stream_old(uint16_t, uint32_t, uint32_t, uint32_t, uint32_t);

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

#define LEN 1024u
int main(int argc, char **argv) {
    uint32_t n = (uint32_t)strtoul(argv[1], 0, 0), repeat = (uint32_t)strtoul(argv[2], 0, 0);
    static double mul[LEN], add[LEN];
    uint32_t i, r;
    g_mem = calloc(1, 0x100000);
    for (i = 0; i < LEN; i++) {
        mul[i] = 1.0 + (double)((i * 2654435761u) % 1000u) * 1e-9 - 5e-7;
        add[i] = (double)((i * 40503u) % 977u) * 1e-6;
        float f = (float)(0.5 + (double)((i * 7919u) % 1000u) * 1e-3);
        memcpy(g_mem + 0x10000 + 4 * i, &f, 4);
    }
    struct { const char *name; uint16_t cw; int old; } paths[] = {
        {"exact x87, fast path (cw 027f)", 0x027f, 0},
        {"double-only helpers (before)   ", 0x027f, 1},
        {"exact x87, PC24 path (cw 007f) ", 0x007f, 0},
        {"exact x87, SoftFloat (cw 037f) ", 0x037f, 0},
    };
    for (unsigned p = 0; p < sizeof paths / sizeof paths[0]; p++) {
        uint32_t nn = paths[p].cw == 0x037f || paths[p].cw == 0x007f ? n / 8 : n;
        double best_chain = 1e9, best_stream = 1e9, sink = 0;
        for (r = 0; r < repeat; r++) {
            double t0 = now();
            sink += paths[p].old ? bench_chain_old(paths[p].cw, mul, add, LEN, nn / 2)
                                 : bench_chain_new(paths[p].cw, mul, add, LEN, nn / 2);
            double t1 = now();
            if (paths[p].old)
                bench_stream_old(paths[p].cw, 0x10000, 0x20000, LEN, nn / 4 / LEN);
            else
                bench_stream_new(paths[p].cw, 0x10000, 0x20000, LEN, nn / 4 / LEN);
            double t2 = now();
            double c = (t1 - t0) * 1e9 / (double)(nn / 2 * 2);
            double s = (t2 - t1) * 1e9 / (double)(nn / 4 / LEN * LEN * 4);
            if (c < best_chain) best_chain = c;
            if (s < best_stream) best_stream = s;
        }
        printf("%s  chain %.3f ns/op  stream %.3f ns/op  (%g)\n", paths[p].name, best_chain,
               best_stream, sink);
    }
    return 0;
}
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old-rev")
    ap.add_argument("--n", type=int, default=200000000, help="x87 instructions per kernel and run")
    ap.add_argument("--repeat", type=int, default=5)
    a = ap.parse_args()
    clang = shutil.which("clang")
    if not clang:
        sys.exit("clang is not on PATH")
    rev = a.old_rev
    if not rev:
        added = subprocess.check_output(["git", "log", "-n1", "--diff-filter=A", "--format=%H", "--",
                                         "runtime/x87.c"], cwd=ROOT, text=True).strip()
        rev = added + "^"
    work = tempfile.mkdtemp(prefix="x87-bench-")
    old = os.path.join(work, "old")
    os.makedirs(old)
    with open(os.path.join(old, "x86.h"), "w") as fh:
        fh.write(subprocess.check_output(["git", "show", "%s:runtime/x86.h" % rev], cwd=ROOT, text=True))
    with open(os.path.join(work, "main.c"), "w") as fh:
        fh.write(MAIN)
    bench = os.path.join(ROOT, "tools/recomp/x87_bench.c")
    flags = ["-O2", "-std=c11", "-ffp-contract=off", "-w"]
    objs = []
    for define, inc, name in (("BENCH_NEW", os.path.join(ROOT, "runtime"), "new.o"),
                              ("BENCH_OLD", old, "old.o")):
        out = os.path.join(work, name)
        subprocess.check_call([clang, "-c", *flags, "-D" + define, "-I", inc, bench, "-o", out])
        objs.append(out)
    exe = os.path.join(work, "x87_bench")
    subprocess.check_call([clang, *flags, "-I", os.path.join(ROOT, "runtime"), os.path.join(work, "main.c"),
                           os.path.join(ROOT, "runtime/x87.c"),
                           os.path.join(ROOT, "third_party/softfloat/softfloat_x87.c"), *objs, "-lm",
                           "-o", exe])
    print("%s, %s; old helpers from %s" % (platform.machine(), platform.platform(), rev))
    subprocess.check_call([exe, str(a.n), str(a.repeat)])


if __name__ == "__main__":
    main()
