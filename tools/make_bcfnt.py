#!/usr/bin/env python3
"""Builds romfs/hanfont.bcfnt from Noto Sans CJK SC (SIL OFL 1.1).

The console's shared font only carries its own menus' characters, so a
translated build shows boxes for the rest. This bakes every character the
catalog uses, plus printable ASCII, into a font in the console's own format,
which citro2d loads with C2D_FontLoad: it then lays text out exactly as it does
with the system font.

    python3 tools/make_bcfnt.py path/to/NotoSansCJKsc-Regular.otf

Three things the format requires, and that a hand-rolled writer gets wrong:

* Glyph n sits at column n%nRows, line n/nRows, but the stride is the cell
  size plus one pixel (libctru reads (index*(cell+1)+1)), so sheets must leave
  that gap or every glyph after the first drifts.
* The sheet is a GPU texture, so its texels go in the console's tiled Morton
  order, not row by row.
* Glyphs are positioned from the cell's top-left, so the baseline has to be
  baked in when drawing, not added later.

Format: https://www.3dbrew.org/wiki/BCFNT
"""

import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]

EM = 26            # ideograph size in pixels
# citro2d draws a font at 30 pixels to the line (30 / cellHeight), so a cell of
# 30 keeps texels and screen pixels one to one and leaves the strokes clean.
CELL = 30
BASELINE = 24      # from the cell's top, in pixels
LINE_FEED = 30     # the line the screens are laid out for
SHEET = 1024       # the console wants power-of-two textures
STRIDE = CELL + 1  # libctru advances a cell plus one pixel
COLS = SHEET // STRIDE
ROWS = SHEET // STRIDE

PLAIN_ASCII = range(0x20, 0x7F)


def tiled(x, y, width):
    """The GPU's texel order: 8x8 tiles, Morton within each."""
    tile = ((y >> 3) * (width >> 3) + (x >> 3)) << 6
    morton = ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) |
              ((x & 4) << 2) | ((y & 4) << 3))
    return tile + morton


def wanted():
    text = (ROOT / "zh" / "zh.tsv").read_text("utf-8")
    chars = {c for c in text
             if 0x3000 <= ord(c) <= 0x9FFF or 0xFF00 <= ord(c) <= 0xFFEF}
    chars.update(chr(c) for c in PLAIN_ASCII)
    return sorted(chars)


def main():
    font_path = sys.argv[1] if len(sys.argv) > 1 else "tools/fonts/NotoSansCJKsc-Regular.otf"
    font = ImageFont.truetype(font_path, EM)
    chars = wanted()
    if len(chars) + 1 > COLS * ROWS:
        sys.exit(f"{len(chars)} glyphs do not fit in {COLS}x{ROWS} cells")

    # Coverage as a mask, in the grey+alpha layout the app's own icon atlas
    # uploads through, so the tiling below matches what the GPU has already
    # been shown to accept. Index 0 stays blank, which is what the character
    # map points at for anything the font does not carry.
    row_texels = [[0] * SHEET for _ in range(SHEET)]

    canvas = Image.new("L", (CELL, CELL), 0)
    draw = ImageDraw.Draw(canvas)
    entries = []   # (codepoint, glyphIndex, advance)
    widths = [(0, 0, 0)]   # glyph 0: blank, no advance

    for index, char in enumerate(chars, start=1):
        col, line = index % COLS, index // COLS
        x0, y0 = col * STRIDE, line * STRIDE
        canvas.paste(0, (0, 0, CELL, CELL))
        # anchor 'ls' puts the glyph's left edge at x and its baseline at y, so
        # descenders fall inside the cell and the ascent is baked in.
        draw.text((1, BASELINE), char, font=font, fill=255, anchor="ls")
        for y in range(CELL):
            row = row_texels[y0 + y]
            row[x0:x0 + CELL] = canvas.crop((0, y, CELL, y + 1)).tobytes()
        advance = max(1, min(255, round(font.getlength(char))))
        bbox = canvas.getbbox()
        glyph_width = 0 if bbox is None else min(255, bbox[2])
        widths.append((0, glyph_width, advance))
        entries.append((ord(char), index, advance))

    # Tiled copy of the sheet: two bytes a texel, white with the coverage in
    # the alpha byte.
    sheet = bytearray(SHEET * SHEET * 2)
    for y in range(SHEET):
        row = row_texels[y]
        for x in range(SHEET):
            value = row[x]
            if value:
                # A texel is 0xFF00 | coverage as the GPU reads it, which on
                # this console lands as the coverage byte first.
                at = tiled(x, y, SHEET) * 2
                sheet[at] = value
                sheet[at + 1] = 0xFF

    # One character map over every range the glyphs use, so a lookup is a
    # subtraction rather than a scan of 730 entries per character drawn.
    cmap_blocks = []
    for begin, end in ((0x0020, 0x9FFF), (0xFF00, 0xFFEF)):
        table = [0] * (end - begin + 1)
        for code, index, _ in entries:
            if begin <= code <= end:
                table[code - begin] = index
        cmap_blocks.append((begin, end, struct.pack(f"<{len(table)}H", *table)))

    cwdh_count = len(widths)
    cwdh_size = 0x10 + 3 * cwdh_count
    cwdh = bytearray(3 * cwdh_count)
    for i, (left, glyph_width, advance) in enumerate(widths):
        cwdh[i * 3:i * 3 + 3] = bytes([left & 0xFF, glyph_width, advance])

    header = 0x14
    finf_size = 0x20
    tglp_size = 0x20
    tglp_off = header + finf_size
    sheet_off = tglp_off + tglp_size
    cwdh_off = sheet_off + len(sheet)
    cmap_off = cwdh_off + cwdh_size
    total = cmap_off + sum(0x14 + len(data) for _, _, data in cmap_blocks)

    out = bytearray(total)
    struct.pack_into("<4sHHIII", out, 0, b"CFNT", 0xFEFF, 0x14, 0x03000000,
                     total, 3 + len(cmap_blocks))

    # FINF. The section pointers name the bytes after each section's magic and
    # size, which is what the console's own fonts do and what libctru adds to.
    struct.pack_into("<4sI", out, header, b"FINF", finf_size)
    out[header + 0x08] = 1                 # font type
    out[header + 0x09] = LINE_FEED         # line feed
    struct.pack_into("<H", out, header + 0x0A, 0)   # alter char: the blank
    out[header + 0x0C:header + 0x0F] = bytes([0, CELL, 0])
    out[header + 0x0F] = 1                 # encoding: Unicode
    struct.pack_into("<III", out, header + 0x10,
                     tglp_off + 8, cwdh_off + 8, cmap_off + 8)
    out[header + 0x1C] = CELL              # height
    out[header + 0x1D] = CELL              # width
    out[header + 0x1E] = BASELINE          # ascent

    # TGLP.
    struct.pack_into("<4sI", out, tglp_off, b"TGLP", tglp_size)
    out[tglp_off + 0x08] = CELL            # cell width
    out[tglp_off + 0x09] = CELL            # cell height
    out[tglp_off + 0x0A] = BASELINE        # baseline position
    out[tglp_off + 0x0B] = CELL            # max character width
    struct.pack_into("<I", out, tglp_off + 0x0C, SHEET * SHEET * 2)  # sheet bytes
    struct.pack_into("<H", out, tglp_off + 0x10, 1)              # sheets
    struct.pack_into("<H", out, tglp_off + 0x12, 5)              # LA8
    struct.pack_into("<H", out, tglp_off + 0x14, COLS)           # glyphs a row
    struct.pack_into("<H", out, tglp_off + 0x16, ROWS)           # glyph rows
    struct.pack_into("<H", out, tglp_off + 0x18, SHEET)
    struct.pack_into("<H", out, tglp_off + 0x1A, SHEET)
    struct.pack_into("<I", out, tglp_off + 0x1C, sheet_off + 8)

    out[sheet_off:cwdh_off] = sheet

    struct.pack_into("<4sI", out, cwdh_off, b"CWDH", cwdh_size)
    struct.pack_into("<HH", out, cwdh_off + 0x08, 0, cwdh_count - 1)
    struct.pack_into("<I", out, cwdh_off + 0x0C, 0)   # last block
    out[cwdh_off + 0x10:cwdh_off + 0x10 + len(cwdh)] = cwdh

    pos = cmap_off
    for number, (begin, end, data) in enumerate(cmap_blocks):
        size = 0x14 + len(data)
        following = pos + size if number + 1 < len(cmap_blocks) else 0
        struct.pack_into("<4sIHHHHI", out, pos, b"CMAP", size, begin, end, 1, 0,
                         following + (8 if following else 0))
        out[pos + 0x14:pos + 0x14 + len(data)] = data
        pos += size

    (ROOT / "romfs").mkdir(exist_ok=True)
    (ROOT / "romfs" / "hanfont.bcfnt").write_bytes(bytes(out))
    print(f"{len(chars)} glyphs, {COLS}x{ROWS} cells, {total} bytes")


if __name__ == "__main__":
    main()
