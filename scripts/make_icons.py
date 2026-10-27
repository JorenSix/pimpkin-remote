"""Draw the app's 10x10, 1-bit icons as PNGs (# = black pixel).

    python3 scripts/make_icons.py

Flipper icons are 1-bit images. The PNG writer below emits a grayscale,
bit-depth-1 file so the FAP asset converter gets exactly what it expects.
"""

import struct
import zlib
from pathlib import Path

ICONS = {
    "pumpkin_10px": [
        "....##....",
        ".....#....",
        ".##.##.##.",
        "#..#..#..#",
        "#.##..##.#",
        "#........#",
        "#.#.##.#.#",
        "#..#..#..#",
        ".#......#.",
        "..######..",
    ],
    # Candle flicker frames for the title pumpkin.
    "pumpkin_10px_1": [
        "....##....",
        ".....#....",
        ".##.##.##.",
        "#..#..#..#",
        "#..#...#.#",
        "#........#",
        "#.#.##.#.#",
        "#..#..#..#",
        ".#......#.",
        "..######..",
    ],
    "pumpkin_10px_2": [
        "....##....",
        ".....#....",
        ".##.##.##.",
        "#..#..#..#",
        "#.##..##.#",
        "#........#",
        "#.######.#",
        "#..####..#",
        ".#......#.",
        "..######..",
    ],
    "pumpkin_10px_3": [
        "...##.....",
        "....#.....",
        ".##.##.##.",
        "#..#..#..#",
        "#.##..##.#",
        "#........#",
        "#.#.##.#.#",
        "#..#..#..#",
        ".#......#.",
        "..######..",
    ],
    # Big cue: a bolt with a plus.
    "bolt_plus_10x10": [
        "...###..#.",
        "..###..###",
        ".###....#.",
        "######....",
        "...###....",
        "..###.....",
        ".###......",
        ".##.......",
        "##........",
        "#.........",
    ],
    # Lightning cue: a bolt without the plus.
    "bolt_10x10": [
        ".....###..",
        "....###...",
        "...###....",
        "..###.....",
        ".#######..",
        ".....###..",
        "....###...",
        "...###....",
        "..##......",
        ".#........",
    ],
    "ghost_10x10": [
        "...####...",
        "..#....#..",
        ".#......#.",
        ".#.##.#.#.",
        ".#.##.#.#.",
        ".#......#.",
        ".#..##..#.",
        ".#......#.",
        ".#.#..#.#.",
        ".##.##.##.",
    ],
    # Crate/box cue.
    "box_10x10": [
        "..######..",
        ".#......#.",
        "#.######.#",
        "#.#....#.#",
        "#.#....#.#",
        "#.#....#.#",
        "#.#....#.#",
        "#.######.#",
        ".#......#.",
        "..######..",
    ],
    # Smoke on: cloud with puffs underneath.
    "cloud_10x10": [
        "...###....",
        "..#...#...",
        ".#.....##.",
        "#........#",
        "#........#",
        ".########.",
        "..........",
        ".#..#..#..",
        "..#..#..#.",
        ".#..#..#..",
    ],
    "cloud_filled_10x10": [
        "...###....",
        "..#####...",
        ".#######..",
        "#########.",
        "#########.",
        "##########",
        "..........",
        ".##.##.##.",
        ".##.##.##.",
        "..........",
    ],
    # Curling "back" arrow used by the exit button.
    "back_arrow_10x8": [
        "..#.......",
        ".##.......",
        "########..",
        ".##.....#.",
        "..#......#",
        ".........#",
        "........#.",
        "...#####..",
    ],
}


def one_bit_png(rows: list[str]) -> bytes:
    """Grayscale bit-depth-1 PNG. 0 = black, 1 = white."""
    w, h = len(rows[0]), len(rows)
    stride = (w + 7) // 8
    raw = bytearray()
    for row in rows:
        raw.append(0)  # PNG filter type: none
        packed = bytearray(stride)
        for x, cell in enumerate(row):
            if cell == ".":
                packed[x // 8] |= 1 << (7 - (x % 8))
        raw.extend(packed)

    def chunk(kind: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + kind
            + data
            + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
        )

    header = struct.pack(">IIBBBBB", w, h, 1, 0, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )


if __name__ == "__main__":
    out = Path(__file__).resolve().parent.parent / "icons"
    out.mkdir(exist_ok=True)
    for name, rows in ICONS.items():
        assert all(len(row) == 10 for row in rows), name
        (out / f"{name}.png").write_bytes(one_bit_png(rows))
        print("\n".join(r.replace(".", "  ").replace("#", "██") for r in rows), f"\n{name}\n")
