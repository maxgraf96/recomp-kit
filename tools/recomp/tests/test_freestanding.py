"""The freestanding build (RECOMP_FREESTANDING, runtime/freestanding.h,
tools/recomp/bundle.py): generated code compiles with no SDK and no C
library, links with ld64.lld alone, and a host dlopen()s the bundle.

    .venv/bin/python -m pytest -q tools/recomp/tests/test_freestanding.py

Needs an LLVM clang (RECOMP_FREESTANDING_CC, Homebrew's llvm, or PATH); the
bundle cases also need ld64.lld (RECOMP_LD64_LLD or Homebrew's lld), and
loading one needs macOS. Each case skips, saying which, when its tool is missing.
"""
import ctypes
import os
import platform
import shutil
import struct
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "tools/recomp"))
sys.path.insert(0, HERE)
import bundle  # noqa: E402
import translate as T  # noqa: E402

DEFINES = ["GUEST_IMAGE_BASE=0x00400000u", "GUEST_HEAP_BASE=0x01000000u", "GUEST_SIZE=0x10000000u"]


def tool(explicit_env, defaults, what):
    try:
        return bundle.find_tool(None, explicit_env, defaults)
    except SystemExit:
        pytest.skip("%s is not installed (brew install llvm lld)" % what)


@pytest.fixture(scope="module")
def cc():
    return tool("RECOMP_FREESTANDING_CC", bundle.DEFAULT_CC, "an LLVM clang")


@pytest.fixture(scope="module")
def lld():
    return tool("RECOMP_LD64_LLD", bundle.DEFAULT_LLD, "ld64.lld")


def test_every_instruction_form_compiles_without_a_c_library(cc, tmp_path):
    """The translator's whole instruction vocabulary - the synthetic cases the
    Unicorn differential suite runs, x87 and SEH forms included - compiles
    with -ffreestanding -nostdinc, and the object asks the host for nothing
    but runtime entry points and the permitted C library remainder."""
    from test_translate_insns import CASES, translate_case
    parts = ['#include "x86.h"', '#include "intrinsics.h"', '#include "seh.h"', ""]
    for case in CASES:
        try:
            parts.append("/* %s */\n%s\n" % (case.name, translate_case(case)))
        except T.TranslateError:
            pass                      # refused forms are test_translate_insns' business
    assert len(parts) > 60                 # the synthetic cases, not an empty file
    source, obj = tmp_path / "synth.c", tmp_path / "synth.o"
    source.write_text("\n".join(parts))
    flags = bundle.compile_flags(cc, tmp_path, tmp_path, DEFINES, "13.0")
    assert "-nostdinc" in flags and "-ffreestanding" in flags
    subprocess.run([cc] + flags + ["-c", str(source), "-o", str(obj)], check=True)
    assert bundle.foreign(bundle.undefined_symbols(obj, cc)) == []


def test_the_policy_names_what_a_bundle_may_not_need():
    assert bundle.foreign(["_recomp_call", "_g_mem", "_memcpy", "_setjmp"]) == []
    assert bundle.foreign(["_fprintf", "_sin", "___stderrp"]) == ["_fprintf", "_sin", "___stderrp"]


def translate_doubler(tmp_path, monkeypatch):
    """translate.main() over one function, fn(x) = 2x, as an executable image."""
    from test_function_db import run_main
    from test_translate_driver import synthetic_image
    entry = 0x00401000
    code = b"\x8b\x44\x24\x04\x01\xc0\xc3"            # MOV EAX,[ESP+4]; ADD EAX,EAX; RET
    img = synthetic_image({entry: code}, base=0x00400000, size=0x2000)
    img.code_pointers = lambda *args, **kwargs: (set(), set())
    monkeypatch.setattr(T, "IMAGE_NAME", "SYNTH.EXE")
    _, gen = run_main(tmp_path, monkeypatch, img, {entry: (
        "00401000  MOV EAX,dword ptr [ESP + 0x4]\n00401004  ADD EAX,EAX\n00401006  RET\n")})
    return gen, entry


def test_the_image_table_registers_itself_in_a_freestanding_build(tmp_path, monkeypatch):
    gen, entry = translate_doubler(tmp_path, monkeypatch)
    table = (gen / "table.c").read_text()
    free = table.split("#ifdef RECOMP_FREESTANDING\n", 1)[1].split("#else\n", 1)[0]
    assert '"SYNTH.EXE", 0x00400000u, 0x00402000u' in free
    assert "recomp_module_register(&recomp_main_module);" in free
    # The linked build's dispatch and fatal path are untouched, and gated.
    linked = table.split("#ifdef RECOMP_FREESTANDING\n", 1)[1].split("#else\n", 1)[1]
    assert "void recomp_call(X86 *c, uint32_t target)" in linked
    assert linked.rstrip().endswith("#endif /* RECOMP_FREESTANDING */")
    assert table.startswith("/* generated") and "#ifndef RECOMP_FREESTANDING\n#include <stdio.h>" in table


# A stand-in host for one bundle: the runtime's data, a module registry, and
# a no-op for every other entry point the bundle names. Only the first
# fields of X86 (r[8], eip) and RecompModule are used; a layout change in
# x86.h shows up as a wrong name or base below, not as silent success.
HOST = r"""
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct { const char *name; uint32_t base, end; const uint32_t *func_addrs;
                 uint32_t func_count; void (*const *base_ptrs)(void *); } Module;
uint8_t *g_mem;
uint32_t g_watch_base, g_watch_len, g_dirty_count, recomp_frame_watch;
uint32_t g_dirty[16];
const int recomp_profile_enabled = 0, recomp_resumable_stacks = 0;
static const Module *registered[8];
static int count;
void recomp_module_register(const Module *m) { registered[count++] = m; }
int host_count(void) { return count; }
const char *host_name(int i) { return registered[i]->name; }
uint32_t host_base(int i) { return registered[i]->base; }
uint32_t host_end(int i) { return registered[i]->end; }
/* Calls entry with one stack argument and the callback sentinel as its return address. */
uint32_t host_call(int i, uint32_t entry, uint32_t arg) {
    const Module *m = registered[i];
    uint32_t c[4096] = {0};
    if (!g_mem) g_mem = calloc(1, 0x01000000);
    for (uint32_t k = 0; k < m->func_count; ++k) {
        if (m->func_addrs[k] != entry) continue;
        uint32_t sp = 0x00800000u, ret = 0x0fdfff00u;
        memcpy(g_mem + sp, &ret, 4);
        memcpy(g_mem + sp + 4, &arg, 4);
        c[4] = sp;
        m->base_ptrs[k](c);
        return c[0];
    }
    return 0xdeadbeefu;
}
%s
"""


def test_a_bundle_links_with_lld_alone_and_a_host_loads_it(tmp_path, monkeypatch, cc, lld):
    gen, entry = translate_doubler(tmp_path, monkeypatch)
    out = tmp_path / "bundles"
    built = bundle.build(gen, out, ["main"], cc, lld, DEFINES, 4, "13.0")
    path = built["main"]
    assert path.is_file()
    undefined = (out / "main.undefined").read_text().split()
    assert bundle.foreign(undefined) == []
    assert "_recomp_module_register" in undefined
    if platform.system() != "Darwin":
        pytest.skip("loading a Mach-O bundle needs macOS")
    # Rebuilding is incremental: nothing changed, nothing compiles.
    assert bundle.build(gen, out, ["main"], cc, lld, DEFINES, 4, "13.0") == built
    data = {"_g_mem", "_g_watch_base", "_g_watch_len", "_g_dirty", "_g_dirty_count",
            "_recomp_frame_watch", "_recomp_profile_enabled", "_recomp_resumable_stacks",
            "_recomp_module_register"}
    stubs = "\n".join("void %s(void) {}" % s[1:] for s in undefined if s not in data and s.startswith("_recomp_"))
    host_c, host_lib = tmp_path / "host.c", tmp_path / "libhost.dylib"
    host_c.write_text(HOST % stubs)
    system_cc = shutil.which("clang") or cc
    subprocess.run([system_cc, "-shared", "-fPIC", "-O1", str(host_c), "-o", str(host_lib)], check=True)
    host = ctypes.CDLL(str(host_lib), mode=ctypes.RTLD_GLOBAL)
    ctypes.CDLL(str(path), mode=ctypes.RTLD_GLOBAL)      # the constructor registers the table
    host.host_name.restype = ctypes.c_char_p
    assert host.host_count() == 1
    assert host.host_name(0) == b"SYNTH.EXE"
    assert (host.host_base(0), host.host_end(0)) == (0x00400000, 0x00402000)
    host.host_call.restype = ctypes.c_uint32
    assert host.host_call(0, ctypes.c_uint32(entry), ctypes.c_uint32(21)) == 42
