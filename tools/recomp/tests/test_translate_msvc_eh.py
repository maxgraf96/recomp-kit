"""MSVC exception frames in the translator, without a game image.

A frame the runtime can resume (runtime/msvc_eh.cpp) - a C++ frame with a try
block, an _except_handler3 frame with an __except scope - gets a checkpoint
where it links itself into FS:[0] and a retirement where it unlinks itself.
Frames that only destroy locals get neither. The funclets and continuations
the frames' tables name are translated entries even when no listing and no
branch names them, and _setjmp3 call sites take the host setjmp in place.
"""
import json
import struct

from test_translate_driver import synthetic_image, translate_entry_fixture
from test_translate_seh import emitted_body
from test_translate_insns import T

BASE = 0x00400000
F, CALLEE, STUB, THUNK, CATCH, UNWIND = (0x401000, 0x401100, 0x401200, 0x401300, 0x401400,
                                         0x401500)
FILTER, SJ_THUNK = 0x401600, 0x401700
SLOT, SEH_SLOT, SJ_SLOT = 0x402000, 0x402004, 0x402008
FUNCINFO, UNWIND_MAP, TRY_MAP, HANDLERS, SCOPE = 0x402100, 0x402200, 0x402300, 0x402400, 0x402500


def u32(v):
    return struct.pack("<I", v & 0xffffffff)


def rel(at, target):
    return struct.pack("<i", target - at)


def cxx_prologue(at, stub, reordered=False):
    """push ebp; mov ebp,esp; push -1; push stub; mov eax,fs:[0]; push eax;
    mov fs:[0],esp - or, as frame-pointer-light code orders it, the load of
    the old head first."""
    load, link = bytes.fromhex("64a100000000"), bytes.fromhex("64892500000000")
    if reordered:
        return bytes.fromhex("558bec") + load + b"\x6a\xff\x68" + u32(stub) + b"\x50" + link
    return bytes.fromhex("558bec6aff") + b"\x68" + u32(stub) + load + b"\x50" + link


def cxx_function(tries, reordered=False):
    """F: a C++ frame calling CALLEE in state 0; returns (bytes, continuation)."""
    code = bytearray(cxx_prologue(F, STUB, reordered))
    code += bytes.fromhex("83ec10" "8965f0" "c745fc00000000")   # sub esp,10h; mov [ebp-10h],esp; state 0
    at = F + len(code)
    code += b"\xe8" + rel(at + 5, CALLEE)                        # call callee
    cont = F + len(code)
    code += bytes.fromhex("c745fcffffffff"                       # state -1   <- the continuation
                          "8b4df4" "64890d00000000"              # mov ecx,[ebp-0Ch]; mov fs:[0],ecx
                          "8be5" "5d" "c3")                      # mov esp,ebp; pop ebp; ret
    return bytes(code), cont


def cxx_image(tries=1, reordered=False):
    body, cont = cxx_function(tries, reordered)
    blocks = {
        F: body,
        CALLEE: b"\xc3",
        STUB: b"\xb8" + u32(FUNCINFO) + b"\xe9" + rel(STUB + 10, THUNK),
        THUNK: b"\xff\x25" + u32(SLOT),
        CATCH: b"\xb8" + u32(cont) + b"\xc3",                    # mov eax,cont; ret
        UNWIND: b"\xc3",
        FUNCINFO: u32(0x19930520) + u32(2) + u32(UNWIND_MAP) + u32(tries) + u32(TRY_MAP),
        UNWIND_MAP: u32(-1) + u32(UNWIND) + u32(-1) + u32(0),
        TRY_MAP: u32(0) + u32(0) + u32(1) + u32(1) + u32(HANDLERS),
        HANDLERS: u32(0) + u32(0) + u32(0) + u32(CATCH),           # catch (...)
    }
    img = synthetic_image(blocks, base=BASE, size=0x3000)
    img.iat_names = {SLOT: "__CxxFrameHandler", SEH_SLOT: "_except_handler3", SJ_SLOT: "_setjmp3"}
    img.code_pointers = lambda *a, **kw: (set(), set())
    return img, blocks, cont


def translate(tmp_path, monkeypatch, img, listed):
    text = translate_entry_fixture(tmp_path, monkeypatch, img, listed)
    return text


def test_a_frame_with_a_try_block_gets_a_checkpoint_and_its_unlink(tmp_path, monkeypatch):
    img, blocks, cont = cxx_image()
    listed = {a: blocks[a] for a in (F, CALLEE, STUB, THUNK)}
    text = translate(tmp_path, monkeypatch, img, listed)
    body = emitted_body(text, F)
    enter = "{ jmp_buf *b_ = recomp_eh_frame_enter(c); if (setjmp(*b_)) { recomp_seh_land(c); return; } }"
    assert body.count(enter) == 1
    assert body.count("recomp_eh_frame_unlink(c);") == 1
    # The checkpoint follows the store that links the frame; the unlink
    # follows the store that unlinks it.
    assert body.index("wr32(c->fs_base") < body.index("recomp_eh_frame_enter")
    assert body.index("recomp_eh_frame_unlink") > body.rindex("wr32(c->fs_base")
    assert "recomp_seh_frame_enter" not in body and "recomp_seh_frame_leave" not in body


def test_the_frame_pointer_light_prologue_is_the_same_frame(tmp_path, monkeypatch):
    img, blocks, _ = cxx_image(reordered=True)
    text = translate(tmp_path, monkeypatch, img, {a: blocks[a] for a in (F, CALLEE, STUB, THUNK)})
    assert "recomp_eh_frame_enter(c)" in emitted_body(text, F)


def test_a_frame_without_try_blocks_is_never_resumed(tmp_path, monkeypatch):
    img, blocks, _ = cxx_image(tries=0)
    text = translate(tmp_path, monkeypatch, img, {a: blocks[a] for a in (F, CALLEE, STUB, THUNK)})
    body = emitted_body(text, F)
    assert "recomp_eh_frame_enter" not in body and "recomp_eh_frame_unlink" not in body
    assert "setjmp" not in body


def test_funclets_and_continuations_are_entries_without_listings(tmp_path, monkeypatch):
    """Nothing branches to a catch block, an unwind action or the address a
    catch block returns; only the FuncInfo names them."""
    img, blocks, cont = cxx_image()
    text = translate(tmp_path, monkeypatch, img, {a: blocks[a] for a in (F, CALLEE, STUB, THUNK)})
    for entry in (CATCH, UNWIND, cont):
        assert "void fn_%08x(X86 *c)" % entry in text
    # The continuation is an alternate entry into the frame's own body.
    assert "void fn_%08x(X86 *c) { body_%08x(c, 0x%xu); }" % (cont, F, cont) in text


def test_the_report_counts_frames_and_names_nothing_missing(tmp_path, monkeypatch):
    img, blocks, cont = cxx_image()
    from test_function_db import run_main
    listings = {}
    img.md.detail = True
    for a in (F, CALLEE, STUB, THUNK):
        listings[a] = "\n".join(img.to_insn(ci).raw for ci in img.md.disasm(blocks[a], a)) + "\n"
    img.md.detail = False
    report, _ = run_main(tmp_path, monkeypatch, img, listings)
    eh = report["msvc_eh"]
    assert eh["frames"] == {"cxx": 1, "cxx_try": 1, "seh3": 0, "seh3_except": 0}
    assert eh["checkpoints"] == 1 and eh["try_blocks"] == 1
    assert eh["entries"] == {"unwind": 1, "catch": 1, "continuation": 1, "stub": 1}
    assert eh["missing"] == []


def seh3_image(with_except=True):
    """F: an _except_handler3 frame, __try { callee() } __except (filter) {...}
    or __try {...} __finally {...}."""
    code = bytearray(bytes.fromhex("558bec6aff") + b"\x68" + u32(SCOPE) + b"\x68" + u32(THUNK))
    code += bytes.fromhex("64a100000000" "50" "64892500000000"   # mov eax,fs:[0]; push eax; link
                          "83ec08" "53" "56" "57" "8965e8"       # sub esp,8; push x3; mov [ebp-18h],esp
                          "c745fc00000000")                      # try level 0
    code += b"\xe8" + rel(F + len(code) + 5, CALLEE)
    code += bytes.fromhex("c745fcffffffff")
    epilogue = F + len(code)
    code += bytes.fromhex("8b4df0" "64890d00000000"              # mov ecx,[ebp-10h]; mov fs:[0],ecx
                          "5f" "5e" "5b" "8be5" "5d" "c3")
    handler = F + len(code)
    code += bytes.fromhex("8b65e8") + b"\xe9" + rel(F + len(code) + 8, epilogue)  # mov esp,[ebp-18h]; jmp
    blocks = {
        F: bytes(code),
        CALLEE: b"\xc3",
        THUNK: b"\xff\x25" + u32(SEH_SLOT),
        FILTER: bytes.fromhex("b801000000c3"),                   # mov eax,1; ret
        SCOPE: u32(-1) + u32(FILTER if with_except else 0) + u32(handler),
    }
    img = synthetic_image(blocks, base=BASE, size=0x3000)
    img.iat_names = {SLOT: "__CxxFrameHandler", SEH_SLOT: "_except_handler3", SJ_SLOT: "_setjmp3"}
    img.code_pointers = lambda *a, **kw: (set(), set())
    return img, blocks, handler, epilogue


def test_an_except_scope_gets_a_checkpoint_and_its_handler_an_entry(tmp_path, monkeypatch):
    img, blocks, handler, epilogue = seh3_image()
    listed = {F: blocks[F][:handler - F], CALLEE: blocks[CALLEE], THUNK: blocks[THUNK]}
    text = translate(tmp_path, monkeypatch, img, listed)
    body = emitted_body(text, F)
    assert body.count("recomp_eh_frame_enter(c)") == 1
    assert body.count("recomp_eh_frame_unlink(c);") == 1
    assert "void fn_%08x(X86 *c)" % FILTER in text
    assert "void fn_%08x(X86 *c)" % handler in text


def test_a_finally_only_frame_is_never_resumed(tmp_path, monkeypatch):
    img, blocks, handler, _ = seh3_image(with_except=False)
    listed = {F: blocks[F][:handler - F], CALLEE: blocks[CALLEE], THUNK: blocks[THUNK]}
    text = translate(tmp_path, monkeypatch, img, listed)
    assert "recomp_eh_frame_enter" not in emitted_body(text, F)
    # The __finally block is still code the runtime calls.
    assert "void fn_%08x(X86 *c)" % handler in text


def test_setjmp3_is_taken_at_the_call_site(tmp_path, monkeypatch):
    """Through its import thunk and through the IAT slot: the host setjmp
    belongs to the live caller, never to a shim that has returned."""
    caller = (b"\x6a\x00\x68" + u32(0x402800)                  # push 0; push buf
              + b"\xe8" + rel(F + 12, SJ_THUNK)                  # call thunk
              + b"\xff\x15" + u32(SJ_SLOT)                       # call [slot]
              + bytes.fromhex("83c408c3"))
    blocks = {F: caller, SJ_THUNK: b"\xff\x25" + u32(SJ_SLOT)}
    img = synthetic_image(blocks, base=BASE, size=0x3000)
    img.iat_names = {SJ_SLOT: "_setjmp3"}
    img.code_pointers = lambda *a, **kw: (set(), set())
    text = translate(tmp_path, monkeypatch, img, blocks)
    body = emitted_body(text, F)
    assert body.count("jmp_buf *b_ = recomp_setjmp3_prepare(c);") == 2
    assert body.count("if (setjmp(*b_) == 0) recomp_setjmp_return(c, 0);") == 2
    assert body.count("else recomp_setjmp3_resume(c);") == 2
    assert "CALL_FN(%08x)" % SJ_THUNK not in body


def listed(img, code, at):
    img.md.detail = True
    insns = [img.to_insn(ci) for ci in img.md.disasm(code, at)]
    img.md.detail = False
    return insns


def test_frames_are_read_from_the_tables_alone():
    """msvc_frames on its own: the frame kind, its table and whether it can be
    resumed, for both prologue orders and both handlers."""
    for reordered in (False, True):
        img, blocks, _ = cxx_image(reordered=reordered)
        insns = listed(img, blocks[F], F)
        fn = T.Function(F, "f", len(blocks[F]), insns)
        fn.measure(img)
        (kind, table, resumable, handler), = T.msvc_frames(fn, img).values()
        assert (kind, table, resumable, handler) == ("cxx", FUNCINFO, True, STUB)
    img, blocks, handler, _ = seh3_image()
    insns = listed(img, blocks[F][:handler - F], F)
    fn = T.Function(F, "f", handler - F, insns)
    fn.measure(img)
    assert list(T.msvc_frames(fn, img).values()) == [("seh3", SCOPE, True, THUNK)]
    assert T.msvc_frame_entries(img, "seh3", SCOPE) == [("filter", FILTER), ("except", handler)]
