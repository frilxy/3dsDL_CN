#!/usr/bin/env python3
"""Reports which characters a CTR font (.bcfnt) can actually draw.

The shared font a console ships is a small subset of CJK, so a translated
build can miss glyphs even when the console is set to that language. This
reads a font's character maps and compares them with the characters the
catalog uses.

    python3 tools/font/coverage.py <font.bcfnt> [--chars FILE]

--chars names a UTF-8 file holding the characters to check, one run of text
(default: zh/zh.tsv's Chinese, which is what the build actually draws).

Format: https://www.3dbrew.org/wiki/BCFNT
"""

import argparse
import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
CJK = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff]")


def sections(data, magic):
    offsets = []
    index = 0
    while True:
        index = data.find(magic, index)
        if index < 0:
            return offsets
        offsets.append(index)
        index += 4


def coverage(data):
    """Every code point the font maps to a glyph."""
    covered = set()
    for offset in sections(data, b"CMAP"):
        if offset + 0x14 > len(data):
            continue
        size, = struct.unpack_from("<I", data, offset + 4)
        begin, end, method = struct.unpack_from("<HHH", data, offset + 8)
        if offset + size > len(data):
            continue
        if method == 0 and size == 0x18:
            # Direct: every code in the range, offset by one index.
            covered.update(range(begin, end + 1))
        elif method == 1:
            count = end - begin + 1
            if 0x14 + count * 2 <= size:
                values = struct.unpack_from(f"<{count}H", data, offset + 0x14)
                covered.update(begin + i for i, v in enumerate(values)
                               if v != 0xFFFF)
        elif method == 2:
            # Scan: a count, then (code, index) pairs.
            count, = struct.unpack_from("<H", data, offset + 0x14)
            base = offset + 0x16
            if base + count * 4 <= offset + size:
                for i in range(count):
                    code, index = struct.unpack_from("<HH", data, base + i * 4)
                    if index != 0xFFFF:
                        covered.add(code)
    return covered


def catalog_chars():
    text = (ROOT / "zh" / "zh.tsv").read_text("utf-8")
    return {c for c in text if CJK.match(c)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("font")
    parser.add_argument("--chars", help="file of characters to check")
    args = parser.parse_args()

    data = pathlib.Path(args.font).read_bytes()
    covered = coverage(data)
    wanted = (set(pathlib.Path(args.chars).read_text("utf-8").strip())
              if args.chars else catalog_chars())
    wanted = {c for c in wanted if CJK.match(c)}

    missing = sorted(c for c in wanted if ord(c) not in covered)
    print(f"{args.font}: maps {len(covered)} code points, "
          f"covers {len(wanted) - len(missing)}/{len(wanted)} needed")
    if missing:
        print("missing:", "".join(missing))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
