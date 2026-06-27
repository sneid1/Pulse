#!/usr/bin/env python3
"""Author the grey-box world textures for Pulse as part of the build.

Writes assets/textures/{wall,floor,ceiling,cover}.ptex. The .ptex format is the
engine's simple authored-texture container: magic "PULSETX1", width (u32 LE),
height (u32 LE), then width*height pixels as u32 0x00RRGGBB. The arena is grey-box
(spec 4.1: no art beyond default grey), so these are deterministic concrete/grid
panels. Grey is channel-order robust, so they read correctly regardless of the
loader's byte order. No randomness module is used; a small integer hash gives
stable per-texel grain.
"""

import struct
import pathlib

SIZE = 256
OUT = pathlib.Path(__file__).resolve().parent.parent / "assets" / "textures"


def grain(x: int, y: int) -> int:
    """Deterministic [-1..1]-ish small integer grain from texel coords."""
    h = (x * 374761393 + y * 668265263) & 0xFFFFFFFF
    h = (h ^ (h >> 13)) * 1274126177 & 0xFFFFFFFF
    return ((h >> 8) & 0x1F) - 16          # -16..15


def clampb(v: int) -> int:
    return 0 if v < 0 else (255 if v > 255 else v)


def write_ptex(path: pathlib.Path, pixels: list[int]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"PULSETX1")
        f.write(struct.pack("<II", SIZE, SIZE))
        f.write(struct.pack("<%dI" % (SIZE * SIZE), *pixels))
    print("wrote %s (%dx%d)" % (path.name, SIZE, SIZE))


def grey(v: int) -> int:
    v = clampb(v)
    return (v << 16) | (v << 8) | v


def tinted(r: int, g: int, b: int) -> int:
    return (clampb(r) << 16) | (clampb(g) << 8) | clampb(b)


def build(kind: str) -> list[int]:
    px = [0] * (SIZE * SIZE)
    for y in range(SIZE):
        for x in range(SIZE):
            n = grain(x, y)
            if kind == "wall":
                # Painted concrete panels, seams every 64 px.
                base = 150 + n // 2
                if x % 64 < 2 or y % 64 < 2:
                    base -= 42
                if (x % 64 < 1) or (y % 64 < 1):
                    base -= 18
                px[y * SIZE + x] = grey(base)
            elif kind == "floor":
                # Tile grid, finer grout.
                base = 128 + n // 2
                if x % 32 < 2 or y % 32 < 2:
                    base -= 40
                px[y * SIZE + x] = grey(base)
            elif kind == "ceiling":
                # Smooth dark grey with faint wide panels.
                base = 116 + n // 3
                if x % 128 < 2 or y % 128 < 2:
                    base -= 20
                px[y * SIZE + x] = grey(base)
            else:  # cover: a cooler accent grey so cover blocks read distinctly
                base = 140 + n // 2
                if (x % 48 < 3) ^ (y % 48 < 3):
                    base -= 30
                px[y * SIZE + x] = tinted(base - 8, base - 2, base + 10)
    return px


def main() -> None:
    for kind in ("wall", "floor", "ceiling", "cover"):
        write_ptex(OUT / (kind + ".ptex"), build(kind))


if __name__ == "__main__":
    main()
