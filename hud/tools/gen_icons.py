#!/usr/bin/env python3
"""Writes hud/images/at_ico_*.png (+ .vtex recipes): white 64x64 icons for the HUD toolkit.

No dependencies: shapes are rasterized here (4x4 supersampling) and written as PNG with zlib.
The CSS shows one with an ico-<name> class (hud/styles/at_kit.css, at_ui.css). Run it after
changing an icon:  python hud/tools/gen_icons.py
"""
import math
import struct
import zlib
from pathlib import Path

OUT = Path(__file__).resolve().parents[1] / "images"
N = 64
SS = 4  # samples per axis per pixel


# A shape is a function (x, y) -> bool on the 64 x 64 canvas (floats).
def circle(cx, cy, r):
    return lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def ring(cx, cy, r, w):
    return lambda x, y: (r - w) ** 2 <= (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def rect(x0, y0, x1, y1):
    return lambda x, y: x0 <= x <= x1 and y0 <= y <= y1


def capsule(x0, y0, x1, y1, w):
    dx, dy = x1 - x0, y1 - y0
    ll = dx * dx + dy * dy

    def f(x, y):
        t = max(0.0, min(1.0, ((x - x0) * dx + (y - y0) * dy) / ll))
        px, py = x0 + t * dx, y0 + t * dy
        return (x - px) ** 2 + (y - py) ** 2 <= (w / 2) ** 2
    return f


def polygon(pts):
    def f(x, y):
        inside = False
        j = len(pts) - 1
        for i in range(len(pts)):
            xi, yi = pts[i]
            xj, yj = pts[j]
            if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
                inside = not inside
            j = i
        return inside
    return f


def union(*s):
    return lambda x, y: any(f(x, y) for f in s)


def minus(a, *holes):
    return lambda x, y: a(x, y) and not any(h(x, y) for h in holes)


def star(cx, cy, r1, r2, n=5):
    pts = []
    for i in range(2 * n):
        r = r1 if i % 2 == 0 else r2
        a = -math.pi / 2 + i * math.pi / n
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return polygon(pts)


ICONS = {
    # Elimination: a skull.
    "elim": minus(
        union(circle(32, 27, 20), rect(21, 34, 43, 50)),
        circle(24, 29, 6), circle(40, 29, 6), polygon([(32, 35), (28, 42), (36, 42)]),
        rect(26.5, 45, 28.5, 51), rect(31, 45, 33, 51), rect(35.5, 45, 37.5, 51)),
    # The bomb went off: a round bomb with a lit fuse.
    "bomb": union(circle(27, 39, 18), capsule(37, 25, 45, 14, 6), circle(49, 10, 4),
                  capsule(54, 6, 57, 3, 2.5), capsule(53, 13, 58, 14, 2.5)),
    # Defused: wire cutters (two crossed jaws and handle loops).
    "defuse": union(capsule(18, 14, 40, 40, 6), capsule(46, 14, 24, 40, 6),
                    ring(20, 48, 9, 4), ring(44, 48, 9, 4)),
    # Time ran out: a clock.
    "time": union(ring(32, 32, 26, 5), capsule(32, 32, 32, 16, 5), capsule(32, 32, 43, 38, 5)),
    # General use.
    "star": star(32, 33, 28, 12),
    "check": union(capsule(12, 34, 26, 48, 8), capsule(26, 48, 52, 18, 8)),
    "cross": union(capsule(16, 16, 48, 48, 8), capsule(48, 16, 16, 48, 8)),
    "pause": union(rect(17, 12, 27, 52), rect(37, 12, 47, 52)),
    "skull": None,  # same as elim (filled below)
}
ICONS["skull"] = ICONS["elim"]


def raster(shape):
    rows = []
    for py in range(N):
        row = bytearray([0])  # filter: none
        for px in range(N):
            hit = 0
            for sy in range(SS):
                for sx in range(SS):
                    if shape(px + (sx + 0.5) / SS, py + (sy + 0.5) / SS):
                        hit += 1
            a = round(255 * hit / (SS * SS))
            row += bytes([255, 255, 255, a])
        rows.append(bytes(row))
    return b"".join(rows)


def png(data):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    ihdr = struct.pack(">IIBBBBB", N, N, 8, 6, 0, 0, 0)  # 8-bit RGBA
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(data, 9)) + chunk(b"IEND", b"")


def vtex(name):
    logo = (OUT / "at_logo.vtex").read_text(encoding="utf-8")
    return logo.replace("panorama/images/custom_game/at_logo.png", f"panorama/images/custom_game/{name}.png")


def main():
    for key, shape in ICONS.items():
        name = f"at_ico_{key}"
        (OUT / f"{name}.png").write_bytes(png(raster(shape)))
        (OUT / f"{name}.vtex").write_text(vtex(name), encoding="utf-8")
        print(f"wrote {name}")


if __name__ == "__main__":
    main()
