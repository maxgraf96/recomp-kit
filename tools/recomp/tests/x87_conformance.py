#!/usr/bin/env python3
"""x87 conformance: hardware-recorded instruction vectors run through translated code.

    .venv/bin/python tools/recomp/tests/x87_conformance.py                 # the committed subset
    .venv/bin/python tools/recomp/tests/x87_conformance.py \\
        --vectors insn.jsonl --results insn-hw.jsonl [--md report.md] [--out mine.jsonl]

RECOMP_X87_VECTORS and RECOMP_X87_RESULTS name a corpus too. The pytest
wrapper is test_x87_conformance.py.

Each vector is one x87 operation on given register and memory inputs under a
given control word, and the results file holds what a real x87 did with it:
the registers left on the stack (80-bit), the memory output of a store, and
the status word. Both files are JSON Lines in the format documented in
data/x87/README.md.

Every operation becomes one small synthetic function made of the very
instruction bytes the recording ran - set the control word, load the inputs
with FLD m80, the operation, FNSTSW, store every register with FSTP m80 - and
goes through the translator as a game function would. The emitted C is built
with the runtime's x87 (runtime/x87.c, SoftFloat) at -O2 without
floating-point contraction, as the game build compiles it, and x87_driver.c
runs every vector through it on one CPU context.

A vector passes when every register output, the memory output and, for the
operations that define them (compares, FXAM, FPREM), C0 to C3 equal the
hardware's bit for bit. The transcendental instructions are reported apart,
with their distance from the hardware in units of the 64-bit significand's
last place: they are not modelled bit for bit. Exception flags and C1 after
arithmetic are best effort and only counted.

Exit status 1 when a non-transcendental vector differs."""

import argparse
import collections
import ctypes as C
import json
import os
import platform
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "tools/recomp"))
import translate as T  # noqa: E402

DATA = os.path.join(HERE, "data", "x87")
DEFAULT_VECTORS = os.path.join(DATA, "insn-subset.jsonl")
DEFAULT_RESULTS = os.path.join(DATA, "insn-subset-hw.jsonl")

# Guest layout for the synthetic functions and their operand block.
CODE_BASE = 0x0D100000
CODE_STRIDE = 0x100
BLOCK = 0x0E200000
STACK = 0x0EFFFF00
MAGIC_RET = 0x0DEAC000

# ------------------------------------------------------------------ ops ---

def _op(lines, regs=0, depth=1, mem=None, out_mem=None, cls="arith"):
    """lines: the operation as (listing text, bytes hex) pairs."""
    return dict(lines=lines, regs=regs, depth=depth, mem=mem, out_mem=out_mem, cls=cls)


def _m(text, code):
    return [(text, code)]


#: Operation name -> its instructions, register inputs (ST(0) = a, ST(1) =
#: b, ST(2) = c), registers left afterwards, memory input and output types,
#: and class. Memory inputs are at [ECX + 8], memory outputs at [ECX + 0x70].
OPS = {
    "fadd": _op(_m("FADD ST0,ST1", "d8c1"), 2, 2), "fsub": _op(_m("FSUB ST0,ST1", "d8e1"), 2, 2),
    "fsubr": _op(_m("FSUBR ST0,ST1", "d8e9"), 2, 2), "fmul": _op(_m("FMUL ST0,ST1", "d8c9"), 2, 2),
    "fdiv": _op(_m("FDIV ST0,ST1", "d8f1"), 2, 2), "fdivr": _op(_m("FDIVR ST0,ST1", "d8f9"), 2, 2),
    "fsqrt": _op(_m("FSQRT", "d9fa"), 1, 1),
    "fadd_m32": _op(_m("FADD dword ptr [ECX + 0x8]", "d84108"), 1, 1, "f32"),
    "fsub_m32": _op(_m("FSUB dword ptr [ECX + 0x8]", "d86108"), 1, 1, "f32"),
    "fsubr_m32": _op(_m("FSUBR dword ptr [ECX + 0x8]", "d86908"), 1, 1, "f32"),
    "fmul_m32": _op(_m("FMUL dword ptr [ECX + 0x8]", "d84908"), 1, 1, "f32"),
    "fdiv_m32": _op(_m("FDIV dword ptr [ECX + 0x8]", "d87108"), 1, 1, "f32"),
    "fdivr_m32": _op(_m("FDIVR dword ptr [ECX + 0x8]", "d87908"), 1, 1, "f32"),
    "fadd_m64": _op(_m("FADD qword ptr [ECX + 0x8]", "dc4108"), 1, 1, "f64"),
    "fsub_m64": _op(_m("FSUB qword ptr [ECX + 0x8]", "dc6108"), 1, 1, "f64"),
    "fsubr_m64": _op(_m("FSUBR qword ptr [ECX + 0x8]", "dc6908"), 1, 1, "f64"),
    "fmul_m64": _op(_m("FMUL qword ptr [ECX + 0x8]", "dc4908"), 1, 1, "f64"),
    "fdiv_m64": _op(_m("FDIV qword ptr [ECX + 0x8]", "dc7108"), 1, 1, "f64"),
    "fdivr_m64": _op(_m("FDIVR qword ptr [ECX + 0x8]", "dc7908"), 1, 1, "f64"),
    "fiadd_m32": _op(_m("FIADD dword ptr [ECX + 0x8]", "da4108"), 1, 1, "i32"),
    "fimul_m32": _op(_m("FIMUL dword ptr [ECX + 0x8]", "da4908"), 1, 1, "i32"),
    "fidiv_m32": _op(_m("FIDIV dword ptr [ECX + 0x8]", "da7108"), 1, 1, "i32"),
    "fisub_m32": _op(_m("FISUB dword ptr [ECX + 0x8]", "da6108"), 1, 1, "i32"),
    "fld_m32": _op(_m("FLD dword ptr [ECX + 0x8]", "d94108"), 0, 1, "f32", cls="load"),
    "fld_m64": _op(_m("FLD qword ptr [ECX + 0x8]", "dd4108"), 0, 1, "f64", cls="load"),
    "fild_m16": _op(_m("FILD word ptr [ECX + 0x8]", "df4108"), 0, 1, "i16", cls="load"),
    "fild_m32": _op(_m("FILD dword ptr [ECX + 0x8]", "db4108"), 0, 1, "i32", cls="load"),
    "fild_m64": _op(_m("FILD qword ptr [ECX + 0x8]", "df6908"), 0, 1, "i64", cls="load"),
    "fld1": _op(_m("FLD1", "d9e8"), cls="const"), "fldl2t": _op(_m("FLDL2T", "d9e9"), cls="const"),
    "fldl2e": _op(_m("FLDL2E", "d9ea"), cls="const"), "fldpi": _op(_m("FLDPI", "d9eb"), cls="const"),
    "fldlg2": _op(_m("FLDLG2", "d9ec"), cls="const"), "fldln2": _op(_m("FLDLN2", "d9ed"), cls="const"),
    "fldz": _op(_m("FLDZ", "d9ee"), cls="const"),
    "fstp_m32": _op(_m("FSTP dword ptr [ECX + 0x70]", "d95970"), 1, 0, out_mem="f32", cls="store"),
    "fstp_m64": _op(_m("FSTP qword ptr [ECX + 0x70]", "dd5970"), 1, 0, out_mem="f64", cls="store"),
    "fistp_m16": _op(_m("FISTP word ptr [ECX + 0x70]", "df5970"), 1, 0, out_mem="i16", cls="store"),
    "fistp_m32": _op(_m("FISTP dword ptr [ECX + 0x70]", "db5970"), 1, 0, out_mem="i32", cls="store"),
    "fistp_m64": _op(_m("FISTP qword ptr [ECX + 0x70]", "df7970"), 1, 0, out_mem="i64", cls="store"),
    "fcom": _op(_m("FCOM ST1", "d8d1"), 2, 2, cls="compare"),
    "fucom": _op(_m("FUCOM ST1", "dde1"), 2, 2, cls="compare"),
    "ftst": _op(_m("FTST", "d9e4"), 1, 1, cls="compare"),
    "fxam": _op(_m("FXAM", "d9e5"), 1, 1, cls="compare"),
    "fcom_m32": _op(_m("FCOM dword ptr [ECX + 0x8]", "d85108"), 1, 1, "f32", cls="compare"),
    "fcom_m64": _op(_m("FCOM qword ptr [ECX + 0x8]", "dc5108"), 1, 1, "f64", cls="compare"),
    "fabs": _op(_m("FABS", "d9e1"), 1, 1, cls="exact"), "fchs": _op(_m("FCHS", "d9e0"), 1, 1, cls="exact"),
    "frndint": _op(_m("FRNDINT", "d9fc"), 1, 1, cls="exact"),
    "fxtract": _op(_m("FXTRACT", "d9f4"), 1, 2, cls="exact"),
    "fscale": _op(_m("FSCALE", "d9fd"), 2, 2, cls="exact"),
    "fprem": _op(_m("FPREM", "d9f8"), 2, 2, cls="exact"),
    "fprem1": _op(_m("FPREM1", "d9f5"), 2, 2, cls="exact"),
    "fsin": _op(_m("FSIN", "d9fe"), 1, 1, cls="transc"), "fcos": _op(_m("FCOS", "d9ff"), 1, 1, cls="transc"),
    "fsincos": _op(_m("FSINCOS", "d9fb"), 1, 2, cls="transc"),
    "fptan": _op(_m("FPTAN", "d9f2"), 1, 2, cls="transc"),
    "fpatan": _op(_m("FPATAN", "d9f3"), 2, 1, cls="transc"),
    "f2xm1": _op(_m("F2XM1", "d9f0"), 1, 1, cls="transc"),
    "fyl2x": _op(_m("FYL2X", "d9f1"), 2, 1, cls="transc"),
    "fyl2xp1": _op(_m("FYL2XP1", "d9f9"), 2, 1, cls="transc"),
    # FLDPI; FMUL m64; FSTP m64: degrees to radians with the 64-bit pi.
    "idiom_fldpi_fmul_m64": _op([("FLDPI", "d9eb"), ("FMUL qword ptr [ECX + 0x8]", "dc4908"),
                                 ("FSTP qword ptr [ECX + 0x70]", "dd5970")], 0, 0, "f64", "f64", cls="idiom"),
}
OP_NAMES = sorted(OPS)

#: Operations whose C0..C3 are part of the result.
CC_OPS = {name for name, o in OPS.items() if o["cls"] == "compare"} | {"fprem", "fprem1"}
CC_MASK = 0x4700


def stub_lines(name):
    """The instruction sequence the hardware recording ran for `name`, as
    (listing text, bytes hex) pairs."""
    o = OPS[name]
    L = [("FNSTCW word ptr [ECX + 0x2]", "d97902"), ("FLDCW word ptr [ECX]", "d96900"),
         ("FNCLEX", "dbe2")]
    for i in reversed(range(o["regs"])):
        L.append(("FLD tword ptr [ECX + 0x%x]" % (0x10 * (i + 1)), "db69%02x" % (0x10 * (i + 1))))
    L += o["lines"]
    L.append(("FNSTSW word ptr [ECX + 0x4]", "dd7904"))
    for k in range(o["depth"]):
        L.append(("FSTP tword ptr [ECX + 0x%x]" % (0x40 + 0x10 * k), "db79%02x" % (0x40 + 0x10 * k)))
    L += [("FNSTSW word ptr [ECX + 0x6]", "dd7906"), ("FNCLEX", "dbe2"),
          ("FLDCW word ptr [ECX + 0x2]", "d96902"), ("RET", "c3")]
    return L


# ------------------------------------------------------------ translate ---

class _Image(object):
    """The synthetic functions' bytes, for the translator's few byte reads."""
    base = end = size = 0
    md = None

    def __init__(self, code):
        self.code = code

    def insn_end(self, addr, mnem):
        return None

    def rd32(self, va):
        return None

    def rd8(self, va):
        return self.code.get(va)

    def is_exec(self, va):
        return False

    def relocated_pointers(self):
        return set()


class _Opts(object):
    eager_flags = False


def translate_ops():
    """C for every operation's function, and {name: guest address}."""
    code, listings, addrs = {}, {}, {}
    for n, name in enumerate(OP_NAMES):
        addr = CODE_BASE + n * CODE_STRIDE
        off, text = 0, []
        for line, hexcode in stub_lines(name):
            b = bytes.fromhex(hexcode)
            for i, x in enumerate(b):
                code[addr + off + i] = x
            text.append("%08x  %s" % (addr + off, line))
            off += len(b)
        assert off <= CODE_STRIDE
        listings[name] = (addr, off, "\n".join(text) + "\n")
        addrs[name] = addr
    image = _Image(code)
    tr = T.Translator(image, set(addrs.values()), _Opts())
    parts = ['#include "x86.h"', ""]
    for name in OP_NAMES:
        addr, size, listing = listings[name]
        fn = T.Function(addr, "x87_" + name, size, T.parse_listing_text(listing))
        fn.measure(_Image({}))
        tr.prepare(fn)
        parts.append("/* %s */\n%s\n" % (name, "\n".join(tr.translate(fn))))
    return "\n".join(parts), addrs


def _table(addrs):
    entries = sorted(addrs.values())
    L = ['#include "x86.h"']
    L += ["void fn_%08x(X86 *c);" % a for a in entries]
    L.append("int recomp_is_call_return(uint32_t target) { return target == 0x%08xu; }" % MAGIC_RET)
    L.append("int32_t recomp_index_of(uint32_t target) { switch (target) {")
    L += ["case 0x%08xu: return %d;" % (a, i) for i, a in enumerate(entries)]
    L.append("default: return -1; } }")
    L.append("void recomp_call(X86 *c, uint32_t target) {\n    switch (target) {")
    L += ["    case 0x%08xu: fn_%08x(c); return;" % (a, a) for a in entries]
    L.append("    default: recomp_unknown_call(c, target);\n    }\n}")
    L.append("void recomp_jump(X86 *c, uint32_t target) {\n"
             "    if (target == 0x%08xu) { c->eip = target; return; }\n"
             "    recomp_call(c, target);\n}" % MAGIC_RET)
    return "\n".join(L) + "\n"


def build(work=None, opt="-O2"):
    """Translate the operations and build them with the runtime's x87 into a
    shared library. Returns (ctypes library, {name: address})."""
    clang = shutil.which("clang")
    if clang is None:
        raise RuntimeError("clang is not on PATH")
    work = work or tempfile.mkdtemp(prefix="x87-conformance-")
    synth, addrs = translate_ops()
    with open(os.path.join(work, "synth.c"), "w") as fh:
        fh.write(synth)
    with open(os.path.join(work, "table.c"), "w") as fh:
        fh.write(_table(addrs))
    lib = os.path.join(work, "libx87conf" + (".dylib" if platform.system() == "Darwin" else ".so"))
    subprocess.check_call([
        clang, opt, "-g", "-std=c11", "-Wall", "-Wextra", "-Wno-unused", "-ffp-contract=off",
        "-fPIC", "-shared", "-I", os.path.join(ROOT, "runtime"),
        os.path.join(HERE, "harness.c"), os.path.join(HERE, "x87_driver.c"),
        os.path.join(work, "synth.c"), os.path.join(work, "table.c"),
        os.path.join(ROOT, "runtime/x87.c"),
        os.path.join(ROOT, "third_party/softfloat/softfloat_x87.c"), "-o", lib])
    dll = C.CDLL(lib)
    dll.harness_mem.restype = C.POINTER(C.c_uint8)
    dll.x87_driver_run.argtypes = [C.c_char_p, C.c_char_p, C.c_uint32, C.c_uint32, C.c_uint32,
                                   C.c_uint32]
    dll.x87_driver_reset.argtypes = [C.c_uint16]
    return dll, addrs


# ----------------------------------------------------------------- data ---

def typed_bytes(tv, size):
    """A typed value ("x80:SSSSMMMM...", "f32:...", "i64:...") as memory bytes."""
    t, _, h = tv.partition(":")
    if t == "x80":
        return struct.pack("<QH", int(h[4:], 16), int(h[:4], 16))
    n = {"f32": 4, "f64": 8, "i16": 2, "i32": 4, "i64": 8}[t]
    return int(h, 16).to_bytes(n, "little").ljust(size, b"\0")


def x80_str(b):
    m, se = struct.unpack("<QH", b)
    return "x80:%04x%016x" % (se, m)


def mem_str(t, b):
    n = {"f32": 4, "f64": 8, "i16": 2, "i32": 4, "i64": 8}[t]
    return "%s:%0*x" % (t, 2 * n, int.from_bytes(b[:n], "little"))


def read_jsonl(path):
    rows = []
    with open(path) as fh:
        for line in fh:
            if line.strip():
                rows.append(json.loads(line))
    return rows


def load(vectors_path, results_path):
    """[(vector, hardware result)] for the instruction vectors that have a result."""
    hw = {}
    for r in read_jsonl(results_path):
        if "meta" not in r:
            hw[r["id"]] = r
    pairs = []
    for v in read_jsonl(vectors_path):
        if v.get("kind", "x87") == "x87" and v["id"] in hw and v["op"] in OPS:
            pairs.append((v, hw[v["id"]]))
    return pairs


IN_SIZE, OUT_SIZE = 48, 44


def run(dll, addrs, vectors):
    """Run the vectors in order on one context; their results in the
    recording's format."""
    buf = bytearray(IN_SIZE * len(vectors))
    zero80 = bytes(10)
    for i, v in enumerate(vectors):
        o = OPS[v["op"]]
        rec = struct.pack("<IH2x", addrs[v["op"]], int(v["cw"], 16))
        rec += b"".join(typed_bytes(v[k], 10) if k in v else zero80 for k in "abc")
        rec += typed_bytes(v["m"], 8) if o["mem"] else bytes(8)
        buf[i * IN_SIZE:i * IN_SIZE + len(rec)] = rec
    out = C.create_string_buffer(OUT_SIZE * len(vectors))
    dll.x87_driver_reset(0x037f)
    dll.x87_driver_run(bytes(buf), out, len(vectors), BLOCK, STACK, MAGIC_RET)
    raw = out.raw
    results = []
    for i, v in enumerate(vectors):
        o = OPS[v["op"]]
        r = raw[i * OUT_SIZE:(i + 1) * OUT_SIZE]
        res = {"id": v["id"], "out": [x80_str(r[10 * k:10 * k + 10]) for k in range(o["depth"])],
               "sw": "%04x" % struct.unpack_from("<H", r, 38)[0],
               "sw_end": "%04x" % struct.unpack_from("<H", r, 40)[0]}
        if o["out_mem"]:
            res["mem"] = mem_str(o["out_mem"], r[30:38])
        results.append(res)
    return results


# -------------------------------------------------------------- compare ---

def ordkey(tv):
    """A totally ordered integer for an 80-bit value (None for a NaN), so the
    difference of two keys counts last-place steps of the 64-bit significand."""
    h = tv[4:]
    se, m = int(h[:4], 16), int(h[4:], 16)
    e = se & 0x7fff
    if e == 0x7fff and m & 0x7fffffffffffffff:
        return None
    k = e * (1 << 63) + (m & 0x7fffffffffffffff) if e else m
    return -k if se >> 15 else k


def ulps(a, b):
    ka, kb = ordkey(a), ordkey(b)
    if ka is None or kb is None:
        return None
    return abs(ka - kb)


def compare(pairs, mine):
    """Per-vector verdicts, grouped. Returns a report dict."""
    groups = collections.OrderedDict()
    totals = collections.Counter()
    transc_dist = collections.Counter()
    examples = []
    for (v, hw), me in zip(pairs, mine):
        o = OPS[v["op"]]
        transc = o["cls"] == "transc"
        key = (v["op"], v["cw"], v.get("tag", "?"))
        g = groups.setdefault(key, collections.Counter())
        g["n"] += 1
        bad = []
        if hw.get("out") != me["out"]:
            bad.append("out")
        if hw.get("mem") != me.get("mem"):
            bad.append("mem")
        if v["op"] in CC_OPS and (int(hw["sw"], 16) ^ int(me["sw"], 16)) & CC_MASK:
            bad.append("cc")
        if transc and (int(hw["sw"], 16) ^ int(me["sw"], 16)) & 0x0400:
            bad.append("c2")
        flags = (int(hw["sw"], 16) ^ int(me["sw"], 16)) & 0x003f
        if flags:
            g["flags"] += 1
            totals["flags"] += 1
        if transc:
            totals["transc"] += 1
            g["transc"] += 1
            if bad:
                worst = 0
                for a, b in zip(hw.get("out") or [], me["out"]):
                    d = ulps(a, b) if a != b else 0
                    worst = "nan" if d is None else max(worst, d) if worst != "nan" else worst
                if len(hw.get("out") or []) != len(me["out"]):
                    worst = "depth"
                bucket = worst if worst in ("nan", "depth") else (
                    "1" if worst <= 1 else "2..4" if worst <= 4 else "5..2^11" if worst < 2048 else ">=2^11")
                transc_dist[bucket] += 1
                totals["transc_diff"] += 1
                g["diff"] += 1
            continue
        totals["n"] += 1
        if bad:
            totals["diff"] += 1
            g["diff"] += 1
            for f in bad:
                g["field_" + f] += 1
            if len(examples) < 40:
                examples.append({"id": v["id"], "op": v["op"], "cw": v["cw"], "tag": v.get("tag"),
                                 "in": {k: v[k] for k in ("a", "b", "c", "m") if k in v},
                                 "hw": {k: hw.get(k) for k in ("out", "mem", "sw")},
                                 "mine": {k: me.get(k) for k in ("out", "mem", "sw")}, "fields": bad})
    return {"groups": groups, "totals": totals, "transc_dist": transc_dist, "examples": examples}


def report_md(rep, vectors_path, results_path):
    t = rep["totals"]
    L = ["# x87 conformance", "",
         "- vectors: `%s`" % vectors_path, "- hardware: `%s`" % results_path,
         "- non-transcendental: %d vectors, **%d differ**" % (t["n"], t["diff"]),
         "- transcendental (informational): %d vectors, %d differ; distance in 64-bit ulps: %s"
         % (t["transc"], t["transc_diff"],
            ", ".join("%s: %d" % kv for kv in sorted(rep["transc_dist"].items())) or "none"),
         "- exception flags differ (best effort, not counted): %d" % t["flags"], "",
         "| op | cw | tag | n | differ | fields | flags |", "|---|---|---|---|---|---|---|"]
    for (op, cw, tag), g in rep["groups"].items():
        fields = ", ".join("%s:%d" % (k[6:], c) for k, c in sorted(g.items()) if k.startswith("field_"))
        L.append("| %s | %s | %s | %d | %d | %s | %d |" % (op, cw, tag, g["n"], g["diff"], fields, g["flags"]))
    if rep["examples"]:
        L += ["", "## Differences (first %d)" % len(rep["examples"]), ""]
        L += ["- `%s`" % json.dumps(e) for e in rep["examples"]]
    return "\n".join(L) + "\n"


def check(vectors_path=None, results_path=None, md=None, out=None, work=None):
    vectors_path = vectors_path or os.environ.get("RECOMP_X87_VECTORS") or DEFAULT_VECTORS
    results_path = results_path or os.environ.get("RECOMP_X87_RESULTS") or DEFAULT_RESULTS
    pairs = load(vectors_path, results_path)
    dll, addrs = build(work)
    mine = run(dll, addrs, [v for v, _ in pairs])
    if out:
        with open(out, "w") as fh:
            fh.write(json.dumps({"meta": {"backend": "recomp-kit translated x87"}}) + "\n")
            for r in mine:
                fh.write(json.dumps(r) + "\n")
    rep = compare(pairs, mine)
    text = report_md(rep, vectors_path, results_path)
    if md:
        with open(md, "w") as fh:
            fh.write(text)
    return rep, text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--vectors")
    ap.add_argument("--results")
    ap.add_argument("--md", help="write the full report here")
    ap.add_argument("--out", help="write this build's results here, in the recording's format")
    ap.add_argument("--work", help="build directory (default: a temporary one)")
    a = ap.parse_args()
    rep, text = check(a.vectors, a.results, a.md, a.out, a.work)
    t = rep["totals"]
    head = text.split("| op |")[0]
    print(head.rstrip())
    for e in rep["examples"][:10]:
        print("  %s" % json.dumps(e))
    sys.exit(1 if t["diff"] else 0)


if __name__ == "__main__":
    main()
