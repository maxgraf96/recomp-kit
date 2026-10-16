"""Program flag liveness ([translate] flag_liveness = "program") against block
liveness and against Unicorn.

    .venv/bin/python -m pytest -q tools/recomp/tests/test_flag_liveness.py

Two kinds of check:
  - the analysis itself, on synthetic bodies: which flags each instruction
    leaves live, through branches, loops, direct calls (what the callee reads
    at its entry, and what passes through a callee that sets no flag on a
    path to its RET), tail jumps, jump tables, indirect transfers and RETs.
    Program liveness may only ever drop flags block liveness keeps: never
    one that some path can read;
  - the emitted code, on branchy synthetic functions run natively and in
    Unicorn from the same registers and memory (test_translate_insns.py's
    harness): registers, the defined arithmetic flags at the RET and memory
    must agree, and the flags block liveness would have computed there too.

No game is needed. clang must be on PATH for the second kind (a skip otherwise).
"""

import ctypes as C
import os
import platform
import random
import shutil
import subprocess
import sys
import tempfile

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "tools/recomp"))
sys.path.insert(0, HERE)
import translate as T  # noqa: E402
import test_translate_insns as I  # noqa: E402

ALL = T.ALL_FLAGS


def body(addr, lines):
    """A Function from (offset, text) lines at `addr`, indexed as translate() needs it."""
    text = "\n".join("%08x  %s" % (addr + off, t) for off, t in lines) + "\n"
    fn = T.Function(addr, "fn_%08x" % addr, max(off for off, _ in lines) + 1,
                    T.parse_listing_text(text))
    fn.measure(I.NoImage())
    return fn


def solve(*bodies, entries=None, jumptables=None):
    """Program liveness over the bodies; {body address: [live-out flag sets]}."""
    tr = T.Translator(I.NoImage(), {fn.addr for fn in bodies}, I.Opts())
    for fn in bodies:
        tr.prepare(fn)
    for key, targets in (jumptables or {}).items():
        tr.jumptables[key] = targets
    tr.prepare_program_liveness(list(bodies), entries or {})
    return {a: [tr.bits_flags(b) for b in live] for a, live in tr.program_live.items()}, tr


def block(fn):
    tr = T.Translator(I.NoImage(), {fn.addr}, I.Opts())
    tr.prepare(fn)
    return tr.liveness(fn)


# -------------------------------------------------------------- analysis --

def test_a_compare_keeps_only_what_its_branch_reads_when_both_paths_overwrite():
    a = 0x0D030000
    fn = body(a, [(0, "CMP EAX,EBX"), (2, "JNZ 0x%08x" % (a + 8)), (4, "XOR EAX,EAX"),
                  (6, "JMP 0x%08x" % (a + 10)), (8, "SUB EAX,EBX"), (10, "RET")])
    live, _ = solve(fn)
    assert live[a][0] == frozenset(("zf",))          # the CMP: only what JNZ reads
    assert block(fn)[0] == ALL                        # block liveness kept all six
    assert live[a][5 - 1] == ALL                      # the SUB before the RET: all six
    assert live[a][2] == ALL                          # the XOR: the RET reads all six


def test_flags_a_loop_reads_on_its_back_edge_stay_live():
    a = 0x0D030100
    fn = body(a, [(0, "XOR EAX,EAX"), (2, "ADC EAX,EDX"), (4, "DEC ECX"),
                  (5, "JNZ 0x%08x" % (a + 2)), (7, "RET")])
    live, _ = solve(fn)
    # DEC leaves CF alone, so ADC's carry from the previous ADC (or the XOR)
    # is read again on the next iteration.
    assert "cf" in live[a][0] and "cf" in live[a][1]
    assert "cf" in live[a][2]                       # through DEC to the back edge


def test_a_direct_call_needs_what_the_callee_reads_at_its_entry():
    a, b = 0x0D030200, 0x0D030300
    caller = body(a, [(0, "TEST EAX,EAX"), (2, "CALL 0x%08x" % b), (7, "XOR ECX,ECX"), (9, "RET")])
    callee = body(b, [(0, "JZ 0x%08x" % (b + 4)), (2, "XOR EAX,EAX"), (4, "XOR EDX,EDX"),
                      (6, "RET")])
    live, tr = solve(caller, callee)
    assert live[a][0] == frozenset(("zf",))           # TEST: the callee's JZ reads ZF
    assert tr.program_stats["entries_reading_flags"] == 1


def test_a_callee_that_sets_no_flag_passes_the_callers_through_to_its_ret():
    a, b = 0x0D030400, 0x0D030500
    caller = body(a, [(0, "CMP EAX,EBX"), (2, "CALL 0x%08x" % b), (7, "RET")])
    callee = body(b, [(0, "MOV EAX,dword ptr [ECX + 0x8]"), (3, "RET")])
    live, _ = solve(caller, callee)
    # The caller's RET reads all six, which come back through the callee
    # unchanged: the CMP computes every one, as block liveness does.
    assert live[a][0] == ALL


def test_a_call_clobbers_flags_a_callee_always_sets():
    a, b = 0x0D030600, 0x0D030700
    caller = body(a, [(0, "CMP EAX,EBX"), (2, "CALL 0x%08x" % b), (7, "RET")])
    callee = body(b, [(0, "XOR EAX,EAX"), (2, "RET")])
    live, _ = solve(caller, callee)
    assert live[a][0] == frozenset()                  # XOR writes all six before the RET


def test_indirect_transfers_and_unknown_targets_keep_all_six():
    a = 0x0D030800
    fn = body(a, [(0, "CMP EAX,EBX"), (2, "CALL EAX"), (4, "CMP EAX,EBX"),
                  (6, "JMP dword ptr [EAX]"), (8, "CMP EAX,EBX"), (10, "JMP 0x0d0ff000")])
    live, _ = solve(fn)
    assert live[a][0] == ALL                         # before an indirect CALL
    assert live[a][2] == ALL                         # before an indirect JMP
    assert live[a][4] == ALL                         # before a jump to no known entry


def test_a_jump_table_reads_its_targets_and_keeps_all_six_for_its_default():
    a = 0x0D030900
    fn = body(a, [(0, "CMP EAX,0x1"), (3, "JA 0x%08x" % (a + 20)),
                  (5, "JMP dword ptr [EAX*0x4 + 0x0d0309f0]"),
                  (12, "XOR EAX,EAX"), (14, "RET"), (16, "XOR ECX,ECX"), (18, "RET"),
                  (20, "XOR EDX,EDX"), (22, "RET")])
    live, _ = solve(fn, jumptables={(a, a + 5): [a + 12, a + 16]})
    assert live[a][0] == ALL                          # the default arm leaves the body


def test_a_tail_jump_and_an_alternate_entry_read_what_their_target_reads():
    a, b = 0x0D030a00, 0x0D030b00
    jumper = body(a, [(0, "TEST EAX,EAX"), (2, "JMP 0x%08x" % (b + 4))])
    target = body(b, [(0, "XOR EAX,EAX"), (2, "NOP"), (3, "NOP"), (4, "JZ 0x%08x" % (b + 8)),
                      (6, "XOR ECX,ECX"), (8, "XOR EDX,EDX"), (10, "RET")])
    live, _ = solve(jumper, target, entries={b: {b + 4}})
    assert live[a][0] == frozenset(("zf",))


def test_program_liveness_never_drops_a_flag_block_liveness_shows_read_in_the_same_run():
    """Straight-line runs: within one, the two analyses agree exactly."""
    a = 0x0D030c00
    fn = body(a, [(0, "ADD EAX,EBX"), (2, "ADC ECX,EDX"), (4, "SBB ESI,ESI"), (6, "INC EDI"),
                  (7, "SETC AL"), (10, "RET")])
    live, _ = solve(fn)
    blk = block(fn)
    assert live[a] == blk


# ------------------------------------------------------------ execution --

def branchy_cases():
    """Synthetic functions whose flags cross branches, joins and a loop."""
    def a(k):
        return 0x0D02F000 + k * 0x100

    def j(base, off):
        return "0x%08x" % (base + off)

    small_ecx = (lambda rng: {"regs": I.rand_regs(rng, ECX=rng.randrange(1, 12))})
    cases = []
    b = a(0)
    cases.append(I.Case(
        "compare, branch, two arithmetic paths, carry read at the join", b,
        [(0, "CMP EAX,EBX"), (2, "JZ " + j(b, 8)), (4, "ADD ECX,EDX"), (6, "JMP " + j(b, 10)),
         (8, "SUB ECX,EDX"), (10, "ADC ESI,EDI"), (12, "RET")],
        "39d8 7404 01d1 eb02 29d1 11fe c3"))
    b = a(1)
    cases.append(I.Case(
        "carry through INC to a JC, SBB on one path, DEC on the other", b,
        [(0, "CMP EAX,EBX"), (2, "INC ECX"), (3, "JC " + j(b, 7)), (5, "DEC EDX"), (6, "RET"),
         (7, "SBB ESI,ESI"), (9, "RET")],
        "39d8 41 7202 4a c3 19f6 c3"))
    b = a(2)
    cases.append(I.Case(
        "a counted loop with a carry chain", b,
        [(0, "XOR EAX,EAX"), (2, "ADC EAX,EDX"), (4, "DEC ECX"), (5, "JNZ " + j(b, 2)),
         (7, "RET")],
        "31c0 11d0 49 75fb c3", small_ecx))
    b = a(3)
    cases.append(I.Case(
        "sign test, conditional negate, SETZ at the join", b,
        [(0, "TEST EAX,EAX"), (2, "JNS " + j(b, 7)), (4, "NEG EAX"), (6, "NOP"),
         (7, "SETZ CL"), (10, "RET")],
        "85c0 7903 f7d8 90 0f94c1 c3"))
    b = a(4)
    cases.append(I.Case(
        "two compares, the first one's flags dead on both paths", b,
        [(0, "CMP EAX,0x10"), (3, "JL " + j(b, 10)), (5, "CMP EBX,ECX"), (7, "SETG DL"),
         (10, "OR EAX,EBX"), (12, "RET")],
        "83f810 7c05 39cb 0f9fc2 09d8 c3"))
    return cases


def translate_program(case):
    old = T.FLAG_LIVENESS
    T.FLAG_LIVENESS = "program"
    try:
        tr = T.Translator(I.NoImage(case), {case.addr}, I.Opts())
        fn = T.Function(case.addr, case.name, len(case.code), T.parse_listing_text(case.listing()))
        fn.measure(I.NoImage())
        tr.prepare(fn)
        tr.prepare_program_liveness([fn], {})
        return "\n".join(tr.translate(fn, case.entries))
    finally:
        T.FLAG_LIVENESS = old


CASES = branchy_cases()


@pytest.fixture(scope="module")
def built():
    clang = shutil.which("clang")
    if clang is None:
        pytest.skip("clang is not on PATH")
    if platform.system() == "Windows":
        pytest.skip("the instruction harness builds as a POSIX shared library")
    parts = ['#include "x86.h"', ""]
    for case in CASES:
        parts.append("/* %s */\n%s\n" % (case.name, translate_program(case)))
    work = tempfile.mkdtemp(prefix="flag-liveness-")
    with open(os.path.join(work, "synth.c"), "w") as fh:
        fh.write("\n".join(parts))
    with open(os.path.join(work, "table.c"), "w") as fh:
        fh.write('#include "x86.h"\n')
        for case in CASES:
            fh.write("void fn_%08x(X86 *c);\n" % case.addr)
        fh.write("int recomp_is_call_return(uint32_t target) { return target == 0x%08xu; }\n"
                 % I.MAGIC_RET)
        fh.write("int32_t recomp_index_of(uint32_t target) { switch (target) {\n")
        for index, case in enumerate(CASES):
            fh.write("case 0x%08xu: return %d;\n" % (case.addr, index))
        fh.write("default: return -1; } }\n")
        fh.write("void recomp_call(X86 *c, uint32_t target) {\n    switch (target) {\n")
        for case in CASES:
            fh.write("    case 0x%08xu: fn_%08x(c); return;\n" % (case.addr, case.addr))
        fh.write("    default: recomp_unknown_call(c, target); c->eip = rd32(c->r[R_ESP]); "
                 "c->r[R_ESP] += 4;\n    }\n}\n")
        fh.write("void recomp_jump(X86 *c, uint32_t target) {\n"
                 "    if (target == 0x%08xu) { c->eip = target; return; }\n"
                 "    recomp_call(c, target);\n}\n" % I.MAGIC_RET)
    lib = os.path.join(work, "libflags" + (".dylib" if platform.system() == "Darwin" else ".so"))
    subprocess.check_call([clang, "-O1", "-g", "-std=c11", "-Wall", "-Wextra", "-Wno-unused",
                           "-ffp-contract=off", "-fPIC", "-shared", "-I", os.path.join(ROOT, "runtime"),
                           os.path.join(HERE, "harness.c"), os.path.join(work, "synth.c"),
                           os.path.join(ROOT, "runtime/x87.c"),
                           os.path.join(ROOT, "third_party/softfloat/softfloat_x87.c"),
                           os.path.join(work, "table.c"), "-o", lib])
    return I.Native(lib)


@pytest.mark.parametrize("case", CASES, ids=[c.name for c in CASES])
def test_program_liveness_runs_as_unicorn_does(case, built):
    emu = I.Emu()
    rng = random.Random(0x666C6167 ^ case.addr)
    for _ in range(200):
        I.run_once(case, built, emu, rng)


@pytest.mark.parametrize("case", CASES, ids=[c.name for c in CASES])
def test_program_liveness_emits_fewer_flags_than_block(case):
    tr = T.Translator(I.NoImage(case), {case.addr}, I.Opts())
    fn = T.Function(case.addr, case.name, len(case.code), T.parse_listing_text(case.listing()))
    fn.measure(I.NoImage())
    tr.prepare(fn)
    blk = "\n".join(tr.translate(fn, case.entries))
    prog = translate_program(case)
    assert prog.count("c->eflags_") <= blk.count("c->eflags_")
