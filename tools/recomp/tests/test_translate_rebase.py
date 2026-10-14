"""A DLL translated for a base other than the one it prefers.

Every DLL of a game may prefer the same ImageBase, and only one of them can
have it; the runtime loader maps the rest at their configured bases and applies
their base relocations (runtime/loader.cpp). The translation must bake in the
same addresses: Image applies the same relocations to the bytes it reads, and
Rebase moves the Ghidra listings, which were exported at the preferred base -
every listed address, every operand a relocation names, and every direct
branch target - while a constant that merely looks like an address stays.
"""
import os
import struct
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE)))
import translate as T  # noqa: E402

PREFERRED = 0x10000000
BASE = 0x10100000
DELTA = BASE - PREFERRED
TEXT, DATA, RELOC = 0x1000, 0x2000, 0x3000


def dll(path, stripped=False):
    """A PE32 DLL: code that loads a global, calls a local function, pushes a
    constant and stores a pointer; data holding a pointer table; .reloc naming
    the three addresses the linker stored."""
    text = bytearray(0x200)
    code = (b"\xa1" + struct.pack("<I", PREFERRED + DATA)          # 1000 MOV EAX,[0x10002000]
            + b"\xe8" + struct.pack("<i", 0x20 - 10)                # 1005 CALL 0x10001020
            + b"\x68" + struct.pack("<I", PREFERRED)                 # 100a PUSH 0x10000000
            + b"\xb9" + struct.pack("<I", PREFERRED + DATA + 4)      # 100f MOV ECX,0x10002004
            + b"\x74\x0b"                                            # 1014 JZ 0x10001021
            + b"\xc3")                                               # 1016 RET
    text[:len(code)] = code
    text[0x20] = 0xc3
    text[0x21] = 0xc3
    data = bytearray(0x200)
    data[0:8] = struct.pack("<II", PREFERRED + TEXT + 0x20, 0x10000000)  # a pointer, a constant
    sites = [TEXT + 1, TEXT + 0x0f + 1, DATA]
    reloc = bytearray()
    for page in sorted({s & ~0xfff for s in sites}):
        entries = [0x3000 | (s & 0xfff) for s in sites if s & ~0xfff == page]
        if len(entries) % 2:
            entries.append(0)
        reloc += struct.pack("<II", page, 8 + 2 * len(entries))
        reloc += b"".join(struct.pack("<H", e) for e in entries)
    reloc_size = len(reloc)
    reloc += bytes(0x200 - len(reloc))

    dos = bytearray(0x80)
    dos[0:2] = b"MZ"
    dos[0x3c:0x40] = struct.pack("<I", 0x80)
    flags = 0x2102 | (0x0001 if stripped else 0)
    coff = struct.pack("<HHIIIHH", 0x14c, 3, 0, 0, 0, 0xe0, flags)
    dirs = bytearray(16 * 8)
    if not stripped:
        dirs[40:48] = struct.pack("<II", RELOC, reloc_size)
    opt = struct.pack("<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII", 0x10b, 0, 0, 0x200, 0x400, 0,
                      TEXT, TEXT, DATA, PREFERRED, 0x1000, 0x200, 4, 0, 0, 0, 4, 0, 0,
                      0x4000, 0x200, 0, 2, 0, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    sections = b""
    for name, rva, raw, fl in ((b".text", TEXT, 0x200, 0x60000020),
                               (b".data", DATA, 0x400, 0xC0000040),
                               (b".reloc", RELOC, 0x600, 0x42000040)):
        sections += struct.pack("<8sIIIIIIHHI", name, 0x200, rva, 0x200, raw, 0, 0, 0, 0, fl)
    header = dos + b"PE\x00\x00" + coff + opt + bytes(dirs) + sections
    header += bytes(0x200 - len(header))
    with open(path, "wb") as fh:
        fh.write(header + bytes(text) + bytes(data) + bytes(reloc))


LISTING = """10001000  MOV EAX,[0x10002000]
10001005  CALL 0x10001020
1000100a  PUSH 0x10000000
1000100f  MOV ECX,0x10002004
10001014  JZ 0x10001021
10001016  RET
"""


def test_the_image_reads_as_the_loader_maps_it(tmp_path):
    path = tmp_path / "a.dll"
    dll(path)
    preferred = T.Image(str(path))
    assert (preferred.base, preferred.preferred_base) == (PREFERRED, PREFERRED)
    assert preferred.rd32(PREFERRED + DATA) == PREFERRED + TEXT + 0x20
    image = T.Image(str(path), BASE)
    assert (image.base, image.preferred_base, image.end) == (BASE, PREFERRED, BASE + 0x4000)
    assert image.rd32(BASE + DATA) == BASE + TEXT + 0x20, "a stored pointer moves"
    assert image.rd32(BASE + DATA + 4) == 0x10000000, "a constant stays"
    assert image.rd32(BASE + TEXT + 1) == BASE + DATA, "an operand the code stores moves"
    assert image.rd32(BASE + TEXT + 0x0b) == PREFERRED, "an immediate no relocation names stays"
    assert image.is_exec(BASE + TEXT) and not image.is_exec(PREFERRED + TEXT)
    assert sorted(image.reloc_sites) == [(PREFERRED + TEXT + 1, PREFERRED + DATA),
                                         (PREFERRED + TEXT + 0x10, PREFERRED + DATA + 4),
                                         (PREFERRED + DATA, PREFERRED + TEXT + 0x20)]
    assert image.relocated_pointers()[BASE + TEXT + 0x20] == BASE + DATA


def test_listings_move_with_the_image(tmp_path):
    path = tmp_path / "a.dll"
    dll(path)
    rebase = T.Rebase(T.Image(str(path), BASE))
    assert rebase.delta == DELTA
    assert rebase.listing(LISTING) == (
        "10101000  MOV EAX,[0x10102000]\n"
        "10101005  CALL 0x10101020\n"
        "1010100a  PUSH 0x10000000\n"
        "1010100f  MOV ECX,0x10102004\n"
        "10101014  JZ 0x10101021\n"
        "10101016  RET\n")
    assert rebase.address(0x10001234) == 0x10101234
    assert rebase.address(0x00401234) == 0x00401234, "outside the image: not this module's"


def test_a_relocated_operand_the_listing_does_not_show_is_an_error(tmp_path):
    """Ghidra printing an address some other way would leave the translation
    calling the preferred base; that has to stop the translation, not pass."""
    path = tmp_path / "a.dll"
    dll(path)
    rebase = T.Rebase(T.Image(str(path), BASE))
    with pytest.raises(T.TranslateError):
        rebase.listing("10001000  MOV EAX,[DAT_10002000]\n")


def test_parse_listing_rebases_when_configured(tmp_path):
    path = tmp_path / "a.dll"
    dll(path)
    listing = tmp_path / "10001000.asm"
    listing.write_text(LISTING)
    saved = T.REBASE
    try:
        T.REBASE = T.Rebase(T.Image(str(path), BASE))
        insns = T.parse_listing(str(listing))
        assert insns[0].addr == 0x10101000 and insns[0].ops == ["EAX", "[0x10102000]"]
        assert T.listed_address(0x10001000) == 0x10101000
        assert T.listing_name(0x10101000) == "10001000"
    finally:
        T.REBASE = saved
    assert T.parse_listing(str(listing))[0].addr == 0x10001000


def test_an_image_without_relocations_cannot_move(tmp_path):
    path = tmp_path / "fixed.dll"
    dll(path, stripped=True)
    assert T.Image(str(path)).base == PREFERRED
    with pytest.raises(T.TranslateError):
        T.Image(str(path), BASE)
