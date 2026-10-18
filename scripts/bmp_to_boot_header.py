#!/usr/bin/env python3
"""Convert a 1-bit BMP into the packed boot-screen header used by BootActivity.

Usage:
  python3 scripts/bmp_to_boot_header.py src/images/boot-rhythm.bmp src/images/BootRhythm.h
  clang-format -i src/images/BootRhythm.h

Output layout (run-length encoded, lossless): rows top to bottom; each row is
a sequence of run lengths alternating white, black, white, ... starting with
white, summing to the image width. A run longer than 255 is written as 255, 0,
<rest> (the zero-length run keeps the colour). Width must be a multiple of 8.
"""

import struct
import sys
from pathlib import Path


def read_bmp(path):
    data = Path(path).read_bytes()
    if data[:2] != b"BM":
        raise SystemExit(f"{path}: not a BMP file")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    header_size = struct.unpack_from("<I", data, 14)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bits_per_pixel = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]
    if bits_per_pixel != 1 or compression != 0:
        raise SystemExit(f"{path}: expected an uncompressed 1-bit BMP, got {bits_per_pixel} bpp")
    if width % 8 != 0:
        raise SystemExit(f"{path}: width {width} must be a multiple of 8")

    palette_offset = 14 + header_size
    luminance = []
    for index in range(2):
        blue, green, red = data[palette_offset + index * 4 : palette_offset + index * 4 + 3]
        luminance.append(0.299 * red + 0.587 * green + 0.114 * blue)
    black_index = 0 if luminance[0] < luminance[1] else 1

    bottom_up = height > 0
    height = abs(height)
    row_bytes = width // 8
    stride = (row_bytes + 3) & ~3
    rows = []
    for y in range(height):
        source_row = height - 1 - y if bottom_up else y
        start = pixel_offset + source_row * stride
        raw = data[start : start + row_bytes]
        # Normalize so a set bit always means black.
        rows.append(raw if black_index == 1 else bytes(b ^ 0xFF for b in raw))
    return width, height, b"".join(rows)


def encode_runs(width, height, packed):
    row_bytes = width // 8
    out = bytearray()
    for y in range(height):
        row = packed[y * row_bytes : (y + 1) * row_bytes]
        black = False
        x = 0
        while x < width:
            run = 0
            while x < width and bool((row[x // 8] >> (7 - x % 8)) & 1) == black:
                run += 1
                x += 1
            while run > 255:
                out += bytes([255, 0])
                run -= 255
            out.append(run)
            black = not black
    return bytes(out)


def decode_runs(width, height, runs):
    """Mirror of the firmware decoder, used to verify the encoding."""
    row_bytes = width // 8
    packed = bytearray(row_bytes * height)
    pos = 0
    for y in range(height):
        x = 0
        black = False
        while x < width:
            run = runs[pos]
            pos += 1
            if black:
                for px in range(x, x + run):
                    packed[y * row_bytes + px // 8] |= 0x80 >> (px % 8)
            x += run
            black = not black
        if x != width:
            raise SystemExit(f"row {y} decodes to {x} pixels, expected {width}")
    if pos != len(runs):
        raise SystemExit("trailing run data")
    return bytes(packed)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    source, target = sys.argv[1], sys.argv[2]
    width, height, packed = read_bmp(source)
    runs = encode_runs(width, height, packed)
    if decode_runs(width, height, runs) != packed:
        raise SystemExit("run-length round trip mismatch")
    lines = []
    for offset in range(0, len(runs), 20):
        chunk = runs[offset : offset + 20]
        lines.append("    " + ", ".join(str(b) for b in chunk) + ",")
    header = "\n".join(
        [
            "#pragma once",
            "#include <cstddef>",
            "#include <cstdint>",
            "",
            f"// Generated from {Path(source).name} by scripts/bmp_to_boot_header.py. Do not edit.",
            "// Per-row run lengths alternating white/black, starting white (see the script).",
            f"constexpr int BOOT_RHYTHM_WIDTH = {width};",
            f"constexpr int BOOT_RHYTHM_HEIGHT = {height};",
            "static const uint8_t BootRhythmRuns[] = {",
            *lines,
            "};",
            "constexpr size_t BOOT_RHYTHM_RUNS_SIZE = sizeof(BootRhythmRuns);",
            "",
        ]
    )
    Path(target).write_text(header)
    print(f"Wrote {target}: {width}x{height}, {len(runs)} bytes ({len(packed)} unpacked)")


if __name__ == "__main__":
    main()
