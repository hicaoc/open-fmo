#!/usr/bin/env python3
"""Import the NRL reference project's Noto Sans SC LVGL font.

The output is a compact, LVGL-independent binary used by Open FMO's small
RGB565 renderer. The source font is GB2312 level-1 plus punctuation, generated
by nrl-esp32/scripts/gen_cjk_font.py from Noto Sans SC (SIL OFL 1.1).
"""

from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path


def array_body(text: str, name: str) -> str:
    match = re.search(rf"\b{name}\[\]\s*=\s*\{{(.*?)\n\}};", text, re.S)
    if not match:
        raise ValueError(f"array {name} not found")
    return match.group(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--source",
        default=r"D:\work\nrl-esp32\src\app\driver\fonts\lv_font_cjk_16.c",
    )
    parser.add_argument(
        "--output",
        default=r"firmware\main\assets\cjk16.bin",
    )
    args = parser.parse_args()

    source = Path(args.source)
    output = Path(args.output)
    text = source.read_text(encoding="utf-8")

    bitmap_text = array_body(text, "glyph_bitmap")
    bitmap = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", bitmap_text))

    dsc_text = array_body(text, "glyph_dsc")
    descriptors = []
    pattern = re.compile(
        r"\.bitmap_index\s*=\s*(\d+).*?\.adv_w\s*=\s*(\d+).*?"
        r"\.box_w\s*=\s*(\d+).*?\.box_h\s*=\s*(\d+).*?"
        r"\.ofs_x\s*=\s*(-?\d+).*?\.ofs_y\s*=\s*(-?\d+)"
    )
    for match in pattern.finditer(dsc_text):
        descriptors.append(tuple(int(value) for value in match.groups()))
    if not descriptors:
        raise ValueError("glyph descriptors not found")

    unicode_lists: dict[int, list[int]] = {}
    for match in re.finditer(
        r"static const uint16_t unicode_list_(\d+)\[\]\s*=\s*\{(.*?)\n\};",
        text,
        re.S,
    ):
        unicode_lists[int(match.group(1))] = [
            int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]+)", match.group(2))
        ]

    codepoint_to_glyph: dict[int, int] = {}
    cmap_pattern = re.compile(
        r"\.range_start\s*=\s*(\d+).*?\.glyph_id_start\s*=\s*(\d+).*?"
        r"unicode_list_(\d+).*?\.list_length\s*=\s*(\d+)",
    )
    for match in cmap_pattern.finditer(array_body(text, "cmaps")):
        range_start, glyph_start, list_index, list_length = map(int, match.groups())
        offsets = unicode_lists[list_index]
        if len(offsets) != list_length:
            raise ValueError(f"cmap {list_index} length mismatch")
        for index, offset in enumerate(offsets):
            codepoint_to_glyph[range_start + offset] = glyph_start + index

    records = bytearray()
    packed_bitmap = bytearray()
    for codepoint, glyph_id in sorted(codepoint_to_glyph.items()):
        bitmap_index, _advance, width, height, offset_x, offset_y = descriptors[glyph_id]
        byte_count = (width * height + 1) // 2
        glyph_bits = bitmap[bitmap_index : bitmap_index + byte_count]
        records.extend(
            struct.pack(
                "<HIBBbb",
                codepoint,
                len(packed_bitmap),
                width,
                height,
                offset_x,
                offset_y,
            )
        )
        packed_bitmap.extend(glyph_bits)

    output.parent.mkdir(parents=True, exist_ok=True)
    header = b"FMOF" + struct.pack("<II", 1, len(codepoint_to_glyph))
    output.write_bytes(header + records + packed_bitmap)
    print(
        f"Imported {len(codepoint_to_glyph)} CJK glyphs: "
        f"{len(packed_bitmap)} bitmap bytes, {output.stat().st_size} total bytes"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

