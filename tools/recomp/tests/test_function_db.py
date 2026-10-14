"""A function database as the translator's authority, and translating a
relocatable module at a base other than its preferred one. No game needed.

    .venv/bin/python -m pytest -q tools/recomp/tests/test_function_db.py
"""
import json
import os
import struct
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "tools/recomp"))
sys.path.insert(0, HERE)
import translate as T  # noqa: E402
from test_translate_driver import synthetic_image  # noqa: E402


def write_db(path, base, **fields):
    doc = {"schema": "recomp-funcdb/1", "module": "synthetic.dll", "sha256": "",
           "image_base": "%#x" % base, "code_ranges": [], "excluded_ranges": [], "entry_points": [],
           "alternate_entries": [], "data_in_code": [], "jump_tables": [], "excluded_functions": []}
    doc.update(fields)
    path.write_text(json.dumps(doc))
    return path


# ------------------------------------------------------------ FunctionDB --

def test_database_addresses_move_to_the_translated_base(tmp_path):
    db = T.FunctionDB(write_db(
        tmp_path / "db.json", 0x10000000,
        code_ranges=[["0x10001000", "0x10003000", ".text"]],
        excluded_ranges=[["0x10002000", "0x10002100", ".drm"]],
        entry_points=[["0x10001000", "function", ["export"]]],
        alternate_entries=[["0x10001010", ["catch_cont"]]],
        data_in_code=[["0x10001800", 8, "table"]],
        excluded_functions=["0x10002000"]), delta=0x00300000)
    assert db.image_base == 0x10000000
    assert set(db.entry_points) == {0x10301000}
    assert set(db.alternate_entries) == {0x10301010}
    assert db.data_in_code == [(0x10301800, 0x10301808)]
    # The excluded range is cut out of the code range it sits in.
    assert db.exec_ranges() == [(0x10301000, 0x10302000, ".text"), (0x10302100, 0x10303000, ".text")]
    assert db.excluded(0x10302000) and db.excluded(0x103020ff) and not db.excluded(0x10302100)
    assert T.FunctionDB.base_of(tmp_path / "db.json") == 0x10000000


def test_a_database_of_another_schema_is_refused(tmp_path):
    path = tmp_path / "db.json"
    path.write_text(json.dumps({"schema": "something-else/1", "image_base": "0x400000"}))
    with pytest.raises(T.TranslateError, match="schema"):
        T.FunctionDB(path)


def test_code_is_classified_by_the_database_not_section_flags(tmp_path):
    """An executable whose sections are all flagged data has code where the
    database says; protection residue flagged code has none."""
    img = synthetic_image({}, base=0x00400000, size=0x4000)
    img.exec_ranges = []                                        # every section "data"
    img.data_ranges = [(0x00401000, 0x00404000, ".text")]
    db = T.FunctionDB(write_db(tmp_path / "db.json", 0x00400000,
                               code_ranges=[["0x401000", "0x403000", ".text"],
                                            ["0x403000", "0x404000", ".drm_t"]],
                               excluded_ranges=[["0x403000", "0x404000", ".drm_t"]]))
    img.use_function_db(db)
    assert img.is_exec(0x00401000) and img.is_exec(0x00402fff)
    assert not img.is_exec(0x00403000)
    assert img.data_ranges == []


# ------------------------------------------------------------ relocation --

def reloc_table(page_rva, offsets):
    """One IMAGE_BASE_RELOCATION block of HIGHLOW entries (plus a pad entry)."""
    entries = [(3 << 12) | o for o in offsets] + [0]
    return struct.pack("<II", page_rva, 8 + 2 * len(entries)) + struct.pack("<%dH" % len(entries), *entries)


def relocatable_image(code_at, sites, base=0x10000000, size=0x4000):
    """A synthetic image with a .reloc block at +0x3000 naming `sites` (VAs)."""
    img = synthetic_image(code_at, base=base, size=size)
    table = reloc_table(0x1000, [s - base - 0x1000 for s in sites])
    data = bytearray(img.data)
    data[0x3000:0x3000 + len(table)] = table
    img.data = bytes(data)
    img.reloc_dir = (0x3000, len(table))
    img.reloc_sites = frozenset(base + rva for rva in T.highlow_sites(img.data, img.reloc_dir, size))
    img.iat_names = {base + 0x2000: "ExitProcess"}
    img.path = "synthetic.dll"
    return img


def test_highlow_sites_reads_the_relocation_table():
    img = relocatable_image({}, [0x10001002, 0x10001ffc])
    assert sorted(T.highlow_sites(img.data, img.reloc_dir, img.size)) == [0x1002, 0x1ffc]


def test_relocating_an_image_moves_every_relocated_dword_and_nothing_else():
    code = {0x10001000: b"\x68" + struct.pack("<I", 0x10001234) + b"\xc3",      # PUSH 0x10001234
            0x10001010: b"\xb8" + struct.pack("<I", 0x10001234) + b"\xc3"}      # MOV EAX, a constant
    img = relocatable_image(code, [0x10001001])
    img.relocate(0x10300000)
    assert img.base == 0x10300000 and img.end == 0x10304000
    assert img.rd32(0x10301001) == 0x10301234                # relocated
    assert img.rd32(0x10301011) == 0x10001234                # not a relocation site
    assert img.reloc_sites == {0x10301001}
    assert img.iat_names == {0x10302000: "ExitProcess"}
    assert img.exec_ranges == [(0x10300000, 0x10304000, ".text")]


def test_high_and_low_halves_move_as_the_loader_moves_them():
    img = synthetic_image({0x10001000: b"\x34\x12\x78\x56"}, base=0x10000000, size=0x4000)
    entries = [(1 << 12) | 0x000, (2 << 12) | 0x002]               # HIGH at +0, LOW at +2
    table = struct.pack("<II", 0x1000, 8 + 2 * len(entries)) + struct.pack("<2H", *entries)
    data = bytearray(img.data)
    data[0x3000:0x3000 + len(table)] = table
    img.data, img.reloc_dir, img.path = bytes(data), (0x3000, len(table)), "synthetic.dll"
    img.reloc_sites, img.iat_names = frozenset(), {}
    img.relocate(0x10300000)
    assert img.rd8(0x10301000) | img.rd8(0x10301001) << 8 == 0x1234 + 0x1030 - 0x1000   # high half
    assert img.rd8(0x10301002) | img.rd8(0x10301003) << 8 == 0x5678                     # low half: +0


def test_an_image_without_relocations_stays_at_its_preferred_base():
    img = synthetic_image({}, base=0x00400000)
    img.reloc_sites, img.path = frozenset(), "synthetic.exe"
    img.relocate(0x00400000)                                    # a no-op
    with pytest.raises(T.TranslateError, match="no base relocations"):
        img.relocate(0x00500000)


def test_a_listing_moves_with_its_relocations():
    """Ghidra exports at the preferred base. Each instruction moves; of its
    operands exactly the fields a relocation covers change, and a direct
    branch target moves because its encoding is relative."""
    A = 0x10001000
    code = (b"\xc7\x05" + struct.pack("<II", 0x10002000, 0x10002000)    # MOV [0x10002000],0x10002000
            + b"\xb9" + struct.pack("<I", 0x10002000)                   # MOV ECX,0x10002000 (constant)
            + b"\xe8" + struct.pack("<i", 0x100)                        # CALL rel32
            + b"\xff\x24\x85" + struct.pack("<I", 0x10002040)           # JMP [EAX*4+0x10002040]
            + b"\xc3")
    img = relocatable_image({A: code}, [A + 2, A + 6, A + 23])
    listing = ("10001000  MOV dword ptr [0x10002000],0x10002000\n"
               "1000100a  MOV ECX,0x10002000\n"
               "1000100f  CALL 0x10001114\n"
               "10001014  JMP dword ptr [EAX*0x4 + 0x10002040]\n"
               "1000101b  RET\n")
    img.relocate(0x10300000)
    moved = T.rebase_listing_text(listing, img, 0x00300000)
    assert moved.splitlines() == [
        "10301000  MOV dword ptr [0x10302000],0x10302000",
        "1030100a  MOV ECX,0x10002000",
        "1030100f  CALL 0x10301114",
        "10301014  JMP dword ptr [EAX*0x4 + 0x10302040]",
        "1030101b  RET",
    ]
    assert T.rebase_listing_text(listing, img, 0) == listing   # nothing to move


def test_a_relocated_field_missing_from_the_listing_is_an_error():
    A = 0x10001000
    img = relocatable_image({A: b"\x68" + struct.pack("<I", 0x10002000)}, [A + 1])
    img.relocate(0x10300000)
    with pytest.raises(T.TranslateError, match="relocated imm 10002000"):
        T.rebase_listing_text("10001000  PUSH 0x10009999\n", img, 0x00300000)


# ---------------------------------------------------------------- driver --

def run_main(tmp_path, monkeypatch, img, functions, function_db=None, load_base=None):
    """translate.main() over `functions` {address: listing text}."""
    listings = tmp_path / "functions"
    listings.mkdir()
    rows = ["address\tname\tsize"]
    for addr, text in sorted(functions.items()):
        (listings / ("%08x.asm" % addr)).write_text(text)
        rows.append("%08x\tfn_%08x\t%d" % (addr, addr, len(text.splitlines())))
    table = tmp_path / "functions.tsv"
    table.write_text("\n".join(rows) + "\n")
    binary, curated = tmp_path / "image", tmp_path / "globals.toml"
    binary.write_bytes(img.data)
    curated.write_text("")
    out, report = tmp_path / "gen", tmp_path / "report.json"
    monkeypatch.setattr(T, "configure", lambda cfg: None)
    monkeypatch.setattr(T.game_config, "load", lambda path: {})
    for name, value in (("LISTINGS", listings), ("FUNCS_TSV", table),
                        ("BINARY", binary), ("CURATED", curated)):
        monkeypatch.setattr(T, name, str(value))
    monkeypatch.setattr(T, "EXTRA_ENTRY_POINTS", frozenset())
    monkeypatch.setattr(T, "FUNCTION_DB", str(function_db) if function_db else None)
    monkeypatch.setattr(T, "LOAD_BASE", load_base)
    monkeypatch.setattr(T, "LISTING_SHIFT", None)
    monkeypatch.setattr(T, "Image", lambda path: img)
    monkeypatch.setattr(sys, "argv", ["translate.py", "--game", str(tmp_path), "--out", str(out),
                                     "--report", str(report), "--quiet"])
    assert T.main() == 0
    return json.loads(report.read_text()), out


def scan_bait(base=0x00400000):
    """Two listed functions, a third nothing calls, and a data dword naming it."""
    f1, f2, hidden, alt = base + 0x1000, base + 0x1010, base + 0x1020, base + 0x1013
    code = {f1: b"\xc3", f2: b"\x90\x90\x90\x90\xc3", hidden: b"\x55\x8b\xec\x5d\xc3",
            base + 0x3000: struct.pack("<I", hidden)}
    img = synthetic_image(code, base=base, size=0x4000)
    img.exec_ranges = [(base + 0x1000, base + 0x2000, ".text")]
    img.data_ranges = [(base + 0x3000, base + 0x3010, ".data")]
    listings = {f1: "%08x  RET\n" % f1,
                f2: "".join("%08x  NOP\n" % (f2 + k) for k in range(4)) + "%08x  RET\n" % (f2 + 4)}
    return img, listings, (f1, f2, hidden, alt)


def test_without_a_database_the_pointer_scan_adopts_what_data_names(tmp_path, monkeypatch):
    img, listings, (f1, f2, hidden, alt) = scan_bait()
    report, _ = run_main(tmp_path, monkeypatch, img, listings)
    assert report["function_db"] is None
    assert report["entry_points_from_data_pointers"] == 1                # `hidden`


def test_with_a_database_nothing_is_adopted_on_a_guess(tmp_path, monkeypatch):
    img, listings, (f1, f2, hidden, alt) = scan_bait()
    db = write_db(tmp_path / "funcdb.json", 0x00400000,
                  code_ranges=[["0x401000", "0x402000", ".text"]],
                  entry_points=[["%#x" % f1, "function", ["export"]], ["%#x" % f2, "function", ["call"]]],
                  alternate_entries=[["%#x" % alt, ["catch_cont"]]],
                  data_in_code=[["%#x" % (hidden + 0x10), 4, "table"]])
    report, out = run_main(tmp_path, monkeypatch, img, listings, function_db=db)
    assert report["entry_points_from_data_pointers"] == 0
    assert report["entry_points_from_immediates"] == 0
    assert report["recovered_blocks"] == []
    assert "%08x" % alt in report["alternate_entries"]                 # seeded from the database
    db_report = report["function_db"]
    assert db_report["functions_not_in_db"] == [] and db_report["entry_points_without_listing"] == []
    table = (out / "table.c").read_text()
    assert "0x%08xu" % hidden not in table and "0x%08xu" % alt in table


def test_a_database_excludes_functions_whatever_the_listing_says(tmp_path, monkeypatch):
    img, listings, (f1, f2, hidden, alt) = scan_bait()
    db = write_db(tmp_path / "funcdb.json", 0x00400000,
                  code_ranges=[["0x401000", "0x402000", ".text"]],
                  entry_points=[["%#x" % f1, "function", ["export"]]],
                  excluded_functions=["%#x" % f2])
    report, out = run_main(tmp_path, monkeypatch, img, listings, function_db=db)
    assert report["function_db"]["listed_but_excluded"] == ["%08x" % f2]
    assert "fn_%08x" % f2 not in "".join(p.read_text() for p in out.glob("chunk_*.c"))


def test_a_module_translates_at_its_load_base(tmp_path, monkeypatch):
    """Listings and database at the preferred base; the translation, the
    report and the dispatch table at the base the module is loaded at."""
    base, load = 0x10000000, 0x10300000
    f1 = base + 0x1000
    code = {f1: b"\x68" + struct.pack("<I", base + 0x1006) + b"\xc3\xc3"}   # PUSH f1+6; RET; RET
    img = relocatable_image(code, [f1 + 1], base=base)
    img.exec_ranges = [(base + 0x1000, base + 0x2000, ".text")]
    db = write_db(tmp_path / "funcdb.json", base, code_ranges=[["%#x" % (base + 0x1000), "%#x" % (base + 0x2000), ".text"]],
                  entry_points=[["%#x" % f1, "function", ["export"]]])
    listings = {f1: "%08x  PUSH 0x%x\n%08x  RET\n" % (f1, f1 + 6, f1 + 5)}
    report, out = run_main(tmp_path, monkeypatch, img, listings, function_db=db, load_base=load)
    assert report["image_base"] == "%08x" % load
    assert report["function_db"]["listing_delta"] == "%08x" % (load - base)
    text = "".join(p.read_text() for p in out.glob("chunk_*.c"))
    assert "void fn_%08x(" % (f1 + 0x300000) in text
    assert "0x%08xu" % (f1 + 0x300006) in text                  # the relocated PUSH operand
    assert "0x%08xu" % (f1 + 6) not in text
