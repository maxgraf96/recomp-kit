#!/usr/bin/env python3
"""Synthetic DirectMusic files for dmusic_tests: no game's music, built here.

    python3 make_dmusic_fixtures.py fixtures/dmusic_fixtures.h          the C header the tests include
    python3 make_dmusic_fixtures.py --check fixtures/dmusic_fixtures.h  fails if that header is stale
    python3 make_dmusic_fixtures.py --dir DIR                           the files themselves, to listen to

What there is, every tone a looped sine so a test can tell them apart by pitch:

  Song.sgt    a style-based segment: 4/4 at 240 beats a minute (a bar is one
              second), four bars long, repeats 0, command track at groove 1,
              a C major chord, the style Song.sty and a band that plays
              program 0 of Tones.dls on performance channel 0. The style and
              the collection are named in upper case (SONG.STY, TONES.DLS), as
              a game's references often are not the files' own case.
  Song.sty    three two-bar patterns over one part each, by groove level:
              groove 1 plays A4 (MIDI 69), groove 2 E5 (76), grooves 3-100 A5
              (81), a held note a bar long in each bar.
  Tones.dls   one instrument, bank 0 program 0: a sine whose period is 50
              samples at 22.05 kHz, 441 Hz at its unity note 69. So the
              grooves sound at 441, 660.7 and 882 Hz.
  GmSong.sgt  the same, but its band takes program 5 from the General MIDI
              collection (gm.dls), which no file here is called.
  GmBank.dls  a stand-in General MIDI collection: program 5, a sine with a
              20-sample period, so A4 sounds at 1102.5 Hz.
  GmBank.sf2  the same instrument as a SoundFont 2 bank.

The layouts are DirectX 6's (the structures a DirectMusic Producer of that era
wrote), and each chunk holds exactly what dmusic reads from it.
"""
import math
import struct
import sys
import uuid
from pathlib import Path

PPQ = 768
BEAT = PPQ          # a quarter note: the time signatures below are in quarters
MEASURE = 4 * BEAT
TEMPO = 240.0
SEGMENT_MEASURES = 4
GRIDS_PER_BEAT = 4
WAVE_RATE = 22050


def guid(n):
    """A fixed GUID per object, so the files are byte-identical every time."""
    return uuid.UUID(int=0x5D3F0000_0000_4000_8000_000000000000 | n).bytes_le


GUID_DEFAULT_GM = uuid.UUID("f17e8673-c3b4-11d1-870b-00600893b1bd").bytes_le
CLSID_STYLE = uuid.UUID("d2ac28a3-b39b-11d1-8704-00600893b1bd").bytes_le  # CLSID_DirectMusicStyle
CLSID_COLLECTION = uuid.UUID("480ff4b0-28b2-11d1-bef7-00c04fbf8fef").bytes_le
TRACK_TEMPO = uuid.UUID("d2ac2885-b39b-11d1-8704-00600893b1bd").bytes_le
TRACK_COMMAND = uuid.UUID("d2ac288c-b39b-11d1-8704-00600893b1bd").bytes_le
TRACK_CHORD = uuid.UUID("d2ac288b-b39b-11d1-8704-00600893b1bd").bytes_le
TRACK_STYLE = uuid.UUID("d2ac288d-b39b-11d1-8704-00600893b1bd").bytes_le
TRACK_BAND = uuid.UUID("d2ac2894-b39b-11d1-8704-00600893b1bd").bytes_le


def chunk(cid, data):
    data = bytes(data)
    pad = b"\0" if len(data) & 1 else b""
    return cid.encode() + struct.pack("<I", len(data)) + data + pad


def lst(kind, *chunks):
    return chunk("LIST", kind.encode() + b"".join(chunks))


def riff(form, *chunks):
    return chunk("RIFF", form.encode() + b"".join(chunks))


def wstr(s):
    return (s + "\0").encode("utf-16-le")


def unfo(name):
    return lst("UNFO", chunk("UNAM", wstr(name)))


def reference(class_id, object_guid, name, file):
    # DMUS_IO_REFERENCE: guidClassID, dwValidData (DMUS_OBJ_OBJECT|CLASS|NAME|FILENAME).
    return lst("DMRF",
               chunk("refh", class_id + struct.pack("<I", 0x1 | 0x2 | 0x4 | 0x10)),
               chunk("guid", object_guid),
               chunk("name", wstr(name)),
               chunk("file", wstr(file)))


def timesig(beats=4, beat=4, grids=GRIDS_PER_BEAT):
    return struct.pack("<BBH", beats, beat, grids)


# ---------------------------------------------------------------------------
# The style
# ---------------------------------------------------------------------------
def style_part(part_guid, midi_note):
    """A two-bar part: one held note per bar, played as a fixed MIDI value."""
    variation_choices = [0x0FFFFFFF] + [0] * 31  # one variation, every chord function
    prth = (timesig() + struct.pack("<32I", *variation_choices) + part_guid +
            struct.pack("<H", 2) +       # wNbrMeasures
            struct.pack("<BBBB", 0, 127, 0, 0))  # bPlayModeFlags FIXED, invert upper/lower, pad
    notes = b""
    for bar in range(2):
        notes += struct.pack("<IIIhHBBBBBB",
                             bar * 4 * GRIDS_PER_BEAT,  # mtGridStart
                             0xFFFFFFFF,                # dwVariation: every variation
                             MEASURE - 72,              # mtDuration: a bar, less a breath
                             0, midi_note, 100,         # nTimeOffset, wMusicValue, bVelocity
                             0, 0, 0, 0, 0)             # ranges, inversion, play mode FIXED
    return lst("part", chunk("prth", prth), chunk("note", struct.pack("<I", 22) + notes))


def style_pattern(name, groove_bottom, groove_top, part_guid):
    ptnh = timesig() + struct.pack("<BBHH", groove_bottom, groove_top, 0, 2)
    prfc = part_guid + struct.pack("<HBBBB", 0, 0, 0, 0, 0)  # logical part 0 = performance channel 0
    return lst("pttn", chunk("ptnh", ptnh), unfo(name), lst("pref", chunk("prfc", prfc)))


def make_style():
    parts = [(guid(0x10), 69, "Low", 1, 1), (guid(0x11), 76, "Mid", 2, 2), (guid(0x12), 81, "High", 3, 100)]
    body = [chunk("styh", timesig() + struct.pack("<d", TEMPO)), chunk("guid", guid(1)), unfo("Song Style")]
    body += [style_part(g, note) for g, note, _, _, _ in parts]
    body += [style_pattern(name, lo, hi, g) for g, _, name, lo, hi in parts]
    return riff("DMST", *body)


# ---------------------------------------------------------------------------
# The segment
# ---------------------------------------------------------------------------
def band(patch, flags, ref):
    # DMUS_IO_INSTRUMENT: dwPatch, dwAssignPatch, dwNoteRanges[4], dwPChannel,
    # dwFlags, bPan, bVolume, nTranspose, dwChannelPriority.
    bins = struct.pack("<II4III", patch, 0, 0, 0, 0, 0, 0, flags) + struct.pack("<BBhI", 64, 100, 0, 0)
    return riff("DMBD", chunk("guid", guid(3)), unfo("Band"), lst("lbil", lst("lbin", chunk("bins", bins), ref)))


def track(class_id, ckid, fcc, body):
    trkh = class_id + struct.pack("<II", 0, 1) + ckid + fcc
    return riff("DMTK", chunk("trkh", trkh), body)


def make_segment(name, instrument):
    length = SEGMENT_MEASURES * MEASURE
    segh = struct.pack("<6I", 0, length, 0, 0, 0, 0)  # repeats, length, play start, loops, resolution
    tempo = chunk("tetr", struct.pack("<I", 16) + struct.pack("<iId", 0, 0, TEMPO))
    command = chunk("cmnd", struct.pack("<I", 12) + struct.pack("<IHBBBBBx", 0, 0, 0, 0, 1, 0, 0))
    chord_name = wstr("C")[:32].ljust(32, b"\0")
    crdb = (struct.pack("<I", 40) + chord_name + struct.pack("<IHBB", 0, 0, 0, 0) +
            struct.pack("<II", 1, 20) +
            struct.pack("<IIIIBBxx", 0x91, 0xAB5, 0xFFFFFF, 0xFFFFFFFF, 0, 0))  # C E G over C major
    chord = lst("cord", chunk("crdh", struct.pack("<I", 0)), chunk("crdb", crdb))
    style = lst("sttr", lst("strf", chunk("stmp", struct.pack("<I", 0)),
                            reference(CLSID_STYLE, guid(1), "Song Style", "SONG.STY")))
    bands = riff("DMBT", lst("lbdl", lst("lbnd", chunk("bdih", struct.pack("<I", 0)), instrument)))
    tracks = lst("trkl",
                 track(TRACK_TEMPO, b"tetr", b"\0\0\0\0", tempo),
                 track(TRACK_COMMAND, b"cmnd", b"\0\0\0\0", command),
                 track(TRACK_CHORD, b"LIST", b"cord", chord),
                 track(TRACK_STYLE, b"LIST", b"sttr", style),
                 track(TRACK_BAND, b"RIFF", b"DMBT", bands))
    return riff("DMSG", chunk("segh", segh), chunk("guid", guid(0x20 + len(name))), unfo(name), tracks)


# ---------------------------------------------------------------------------
# DLS and SoundFont
# ---------------------------------------------------------------------------
def sine(period, cycles):
    return [int(round(16000 * math.sin(2 * math.pi * i / period))) for i in range(period * cycles)]


def wsmp(unity, frames):
    # WSMPL + one WLOOP over the whole wave.
    return chunk("wsmp", struct.pack("<IHhiII", 20, unity, 0, 0, 0, 1) + struct.pack("<IIII", 16, 0, 0, frames))


def make_dls(name, bank, program, period, cycles, dlid):
    samples = sine(period, cycles)
    pcm = struct.pack("<%dh" % len(samples), *samples)
    region = lst("rgn ",
                 chunk("rgnh", struct.pack("<HHHHHH", 0, 127, 0, 127, 0, 0)),
                 wsmp(69, len(samples)),
                 chunk("wlnk", struct.pack("<HHII", 0, 0, 1, 0)))
    instrument = lst("ins ",
                     chunk("insh", struct.pack("<III", 1, bank, program)),
                     chunk("dlid", guid(dlid + 1)),
                     lst("lrgn", region),
                     lst("INFO", chunk("INAM", (name + "\0").encode())))
    fmt = chunk("fmt ", struct.pack("<HHIIHH", 1, 1, WAVE_RATE, WAVE_RATE * 2, 2, 16))  # PCMWAVEFORMAT
    wave = lst("wave", fmt, wsmp(69, len(samples)), chunk("data", pcm))
    return riff("DLS ",
                chunk("colh", struct.pack("<I", 1)),
                chunk("dlid", guid(dlid)),
                lst("lins", instrument),
                chunk("ptbl", struct.pack("<III", 8, 1, 0)),
                lst("wvpl", wave),
                lst("INFO", chunk("INAM", (name + "\0").encode())))


def make_sf2(name, bank, program, period, cycles):
    samples = sine(period, cycles)
    smpl = struct.pack("<%dh" % len(samples), *samples) + b"\0" * 92  # 46 zero samples after each
    n = len(samples)

    def name20(s):
        return s.encode()[:19].ljust(20, b"\0")

    phdr = (name20(name) + struct.pack("<HHHIII", program, bank, 0, 0, 0, 0) +
            name20("EOP") + struct.pack("<HHHIII", 0, 0, 1, 0, 0, 0))
    pbag = struct.pack("<HH", 0, 0) + struct.pack("<HH", 1, 0)
    pmod = b"\0" * 10
    pgen = struct.pack("<HH", 41, 0) + struct.pack("<HH", 0, 0)  # instrument 0
    inst = name20(name) + struct.pack("<H", 0) + name20("EOI") + struct.pack("<H", 1)
    ibag = struct.pack("<HH", 0, 0) + struct.pack("<HH", 3, 0)
    imod = b"\0" * 10
    igen = (struct.pack("<HH", 54, 1) +   # sampleModes: loop continuously
            struct.pack("<HH", 58, 69) +  # overridingRootKey
            struct.pack("<HH", 53, 0) +   # sampleID 0, last as the format requires
            struct.pack("<HH", 0, 0))
    shdr = (name20(name) + struct.pack("<IIIIIBbHH", 0, n, 0, n, WAVE_RATE, 69, 0, 0, 1) +
            name20("EOS") + struct.pack("<IIIIIBbHH", 0, 0, 0, 0, 0, 0, 0, 0, 0))
    info = lst("INFO", chunk("ifil", struct.pack("<HH", 2, 1)), chunk("isng", b"EMU8000\0"),
               chunk("INAM", (name + "\0").encode()))
    pdta = lst("pdta", chunk("phdr", phdr), chunk("pbag", pbag), chunk("pmod", pmod), chunk("pgen", pgen),
               chunk("inst", inst), chunk("ibag", ibag), chunk("imod", imod), chunk("igen", igen),
               chunk("shdr", shdr))
    return riff("sfbk", info, lst("sdta", chunk("smpl", smpl)), pdta)


def fixtures():
    tones_ref = reference(CLSID_COLLECTION, guid(2), "Tones", "TONES.DLS")
    gm_ref = reference(CLSID_COLLECTION, GUID_DEFAULT_GM, "GM", "gm.dls")
    return {
        "Song.sgt": make_segment("Song", band(0x00000000, 0x1 | 0x20 | 0x40, tones_ref)),
        "GmSong.sgt": make_segment("GmSong", band(0x00000005, 0x1 | 0x20 | 0x40 | 0x100, gm_ref)),
        "Song.sty": make_style(),
        "Tones.dls": make_dls("Sine", 0, 0, 50, 10, 0x30),
        "GmBank.dls": make_dls("GM Sine", 0, 5, 20, 25, 0x40),
        "GmBank.sf2": make_sf2("GM Sine", 0, 5, 20, 25),
    }


def header(files):
    out = ["// dmusic_fixtures.h - synthetic DirectMusic files for dmusic_tests.",
           "// Generated by dx/tests/make_dmusic_fixtures.py, which says what they hold; do not edit.",
           "#pragma once",
           "#include <stddef.h>",
           "#include <stdint.h>",
           ""]
    names = []
    for i, (name, data) in enumerate(sorted(files.items())):
        ident = "kDmFixture%d" % i
        names.append((name, ident, len(data)))
        out.append("static const uint8_t %s[%d] = {" % (ident, len(data)))
        for at in range(0, len(data), 16):
            out.append("    " + ", ".join("0x%02x" % b for b in data[at:at + 16]) + ",")
        out.append("};")
    out.append("")
    out.append("struct DmFixture {")
    out.append("    const char *name;")
    out.append("    const uint8_t *data;")
    out.append("    size_t size;")
    out.append("};")
    out.append("// clang-format off")
    out.append("static const DmFixture kDmFixtures[] = {")
    for name, ident, size in names:
        out.append('    {"%s", %s, %d},' % (name, ident, size))
    out.append("};")
    out.append("// clang-format on")
    return "\n".join(out) + "\n"


def main(argv):
    files = fixtures()
    if len(argv) == 3 and argv[1] == "--dir":
        directory = Path(argv[2])
        directory.mkdir(parents=True, exist_ok=True)
        for name, data in files.items():
            (directory / name).write_bytes(data)
        return 0
    if len(argv) == 3 and argv[1] == "--check":
        if Path(argv[2]).read_text() != header(files):
            print("%s is not what make_dmusic_fixtures.py writes; regenerate it" % argv[2])
            return 1
        return 0
    if len(argv) == 2:
        Path(argv[1]).write_text(header(files))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
