# A TrueType font with the tables Windows GDI measures text with - OS/2
# (version 2), hdmx and VDMX, post and a full name table - for the GDI
# metrics tests. Two faces of the family "WinMetricTest": Regular (400) and
# Bold (700). 2048 units per em, Windows ascent 1854 and descent 434 (the
# proportions of Arial), line gap 67.
#
# The tables are chosen so a test can tell which one GDI's rules used:
#   - hdmx has records for 11, 12 and 16 pixels per em, and at 12 it gives
#     'A' 9 pixels where the linear advance (1366 * 12 / 2048) rounds to 8;
#   - VDMX (ratio 1:1) covers 8 to 16 pixels per em, and at 14 gives an
#     ascent of 15 where the Windows ascent alone would round to 13.
#
#   python3 make_gdi_font.py out.h      writes the C header the tests include
#   python3 make_gdi_font.py --ttf dir  writes the two .ttf files into dir
import struct, sys

UPEM = 2048
WIN_ASCENT, WIN_DESCENT = 1854, 434

def box(x0, y0, x1, y1):
    flags = bytes([1, 1, 1, 1])
    xs = [x0, x1 - x0, 0, x0 - x1]
    ys = [y0, 0, y1 - y0, 0]
    return (struct.pack('>hhhhh', 1, x0, y0, x1, y1) + struct.pack('>HH', 3, 0) + flags +
            b''.join(struct.pack('>h', v) for v in xs) + b''.join(struct.pack('>h', v) for v in ys))

# glyph id: (code point, advance, outline box or None)
GLYPHS = [
    (None, 1536, (128, 0, 1408, 1466)),   # .notdef
    (0x20, 569, None),                    # space
    (0x41, 1366, (0, 0, 1366, 1466)),     # A
    (0x42, 1366, (150, 0, 1250, 1466)),   # B
    (0x69, 455, (136, 0, 316, 1466)),     # i
    (0x20AC, 1139, (40, -20, 1100, 1480)),  # euro sign
]
# ppem -> widths by glyph id; 'A' at 12 is 9, not the linear 8.
HDMX = {
    11: [8, 3, 7, 7, 2, 6],
    12: [9, 3, 9, 8, 3, 7],
    16: [12, 4, 11, 11, 4, 9],
}
# ppem -> (yMax, yMin)
VDMX = {8: (8, -2), 9: (9, -3), 10: (10, -3), 11: (11, -3), 12: (12, -3), 13: (13, -3),
        14: (15, -3), 15: (15, -4), 16: (16, -4)}

def font(bold):
    glyf, loca = b'', [0]
    for _, _, outline in GLYPHS:
        g = box(*outline) if outline else b''
        g += b'\0' * (-len(g) % 4)
        glyf += g
        loca.append(len(glyf))
    n = len(GLYPHS)
    t = {}
    t[b'head'] = struct.pack('>IIIIHHQQhhhhHHhhh', 0x00010000, 0x00010000, 0, 0x5F0F3CF5,
                             0x001b, UPEM, 0, 0, -1361, -431, 4096, 1854,
                             1 if bold else 0, 8, 2, 1, 0)
    t[b'hhea'] = struct.pack('>IhhhHhhhhhh4hhH', 0x00010000, WIN_ASCENT, -WIN_DESCENT, 67, 1536,
                             0, 0, 1408, 1, 0, 0, 0, 0, 0, 0, 0, n)
    t[b'maxp'] = struct.pack('>IH', 0x00005000, n)
    t[b'hmtx'] = b''.join(struct.pack('>Hh', adv, 0) for _, adv, _ in GLYPHS)
    t[b'loca'] = b''.join(struct.pack('>I', o) for o in loca)
    t[b'glyf'] = glyf
    # cmap format 4: one segment per mapped character, then the terminator.
    chars = sorted((cp, gid) for gid, (cp, _, _) in enumerate(GLYPHS) if cp is not None)
    segs = [(cp, cp, (gid - cp) & 0xFFFF) for cp, gid in chars] + [(0xFFFF, 0xFFFF, 1)]
    nseg = len(segs)
    # The binary-search fields readers (stb_truetype among them) rely on.
    power = 1
    while power * 2 <= nseg:
        power *= 2
    selector = power.bit_length() - 1
    sub = struct.pack('>HHHHHHH', 4, 16 + 8 * nseg, 0, 2 * nseg, 2 * power, selector,
                      2 * nseg - 2 * power)
    sub += b''.join(struct.pack('>H', e) for _, e, _ in segs) + b'\0\0'
    sub += b''.join(struct.pack('>H', s) for s, _, _ in segs)
    sub += b''.join(struct.pack('>H', d) for _, _, d in segs)
    sub += b'\0\0' * nseg
    t[b'cmap'] = struct.pack('>HHHHI', 0, 1, 3, 1, 12) + sub
    weight = 700 if bold else 400
    panose = bytes([2, 11, 6, 4, 2, 2, 2, 2, 2, 4])
    t[b'OS/2'] = (struct.pack('>HhHHH', 2, 904, weight, 5, 0) +
                  struct.pack('>hhhhhhhh', 1434, 1331, 0, 286, 1434, 1331, 0, 983) +
                  struct.pack('>hhh', 102, 530, 0x0805) + panose + b'\0' * 16 + b'TEST' +
                  struct.pack('>HHH', 0x20 if bold else 0x40, 0x20, 0x20AC) +
                  struct.pack('>hhhHH', 1491, -431, 307, WIN_ASCENT, WIN_DESCENT) +
                  struct.pack('>II', 1, 0) + struct.pack('>hhHHH', 1062, 1467, 0, 0x20, 3))
    t[b'post'] = struct.pack('>IIhhIIIII', 0x00030000, 0, -217, 150, 0, 0, 0, 0, 0)
    rec = 2 + n
    rec += -rec % 4
    hd = struct.pack('>Hhi', 0, len(HDMX), rec)
    for ppem, widths in sorted(HDMX.items()):
        body = bytes([ppem, max(widths)]) + bytes(widths)
        hd += body + b'\0' * (rec - len(body))
    t[b'hdmx'] = hd
    group = struct.pack('>HBB', len(VDMX), min(VDMX), max(VDMX))
    group += b''.join(struct.pack('>Hhh', p, hi, lo) for p, (hi, lo) in sorted(VDMX.items()))
    t[b'VDMX'] = struct.pack('>HHH', 1, 1, 1) + bytes([1, 1, 1, 1]) + \
        struct.pack('>H', 6 + 4 + 2) + group
    style = 'Bold' if bold else 'Regular'
    names = {1: 'WinMetricTest', 2: style, 4: 'WinMetricTest Bold' if bold else 'WinMetricTest'}
    strings, records = b'', b''
    for nid, text in sorted(names.items()):
        data = text.encode('utf-16-be')
        records += struct.pack('>HHHHHH', 3, 1, 0x409, nid, len(data), len(strings))
        strings += data
    t[b'name'] = struct.pack('>HHH', 0, len(names), 6 + 12 * len(names)) + records + strings
    tags = sorted(t)
    out = struct.pack('>IHHHH', 0x00010000, len(tags), 8 * 4, 3, 16 * len(tags) - 32)
    offset = 12 + 16 * len(tags)
    body = b''
    for tag in tags:
        data = t[tag]
        out += tag + struct.pack('>III', 0, offset + len(body), len(data))
        body += data + b'\0' * (-len(data) % 4)
    return out + body

def c_array(name, data):
    lines = ['static const uint8_t %s[] = {' % name]
    for i in range(0, len(data), 16):
        lines.append('    ' + ' '.join('0x%02x,' % b for b in data[i:i + 16]))
    lines.append('};')
    return '\n'.join(lines)

if __name__ == '__main__':
    regular, bold = font(False), font(True)
    if sys.argv[1] == '--ttf':
        open(sys.argv[2] + '/WinMetricTest.ttf', 'wb').write(regular)
        open(sys.argv[2] + '/WinMetricTest Bold.ttf', 'wb').write(bold)
    else:
        with open(sys.argv[1], 'w') as fh:
            fh.write('// Generated by make_gdi_font.py: the "WinMetricTest" family, Regular and\n'
                     '// Bold, with OS/2 v2, hdmx and VDMX tables for the GDI metrics tests.\n'
                     '#pragma once\n#include <cstdint>\n')
            fh.write(c_array('kGdiFontRegular', regular) + '\n')
            fh.write(c_array('kGdiFontBold', bold) + '\n')
