"""Generates the PWA icons in push/public/icons (standard library only).

Opaque dark background (iOS fills transparency with black anyway) and the same
ring gauge as the Windows app, kept inside the maskable safe zone.
"""
import math
import os
import struct
import zlib

BG = (20, 20, 20)
CLAY = (217, 119, 87)
TRACK = (70, 70, 70)
FILL = 0.68


def render(size, ring_scale):
    c = size / 2.0
    r_out = size * ring_scale
    r_in = r_out * 0.62
    ss = 3
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            acc = [0.0, 0.0, 0.0]
            for sy in range(ss):
                for sx in range(ss):
                    px = x + (sx + 0.5) / ss - c
                    py = y + (sy + 0.5) / ss - c
                    d = math.hypot(px, py)
                    col = BG
                    if r_in <= d <= r_out:
                        ang = math.atan2(px, -py)
                        if ang < 0:
                            ang += 2 * math.pi
                        col = CLAY if ang / (2 * math.pi) < FILL else TRACK
                    for i in range(3):
                        acc[i] += col[i]
            n = ss * ss
            row += bytes(round(v / n) for v in acc)
        rows.append(bytes(row))
    return rows


def png(size, rows):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    raw = b"".join(b"\0" + r for r in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "public", "icons")
    os.makedirs(out, exist_ok=True)
    icons = {
        "apple-touch-icon.png": (180, 0.30),
        "icon-192.png": (192, 0.30),
        "icon-512.png": (512, 0.30),
        "icon-512-maskable.png": (512, 0.26),
    }
    for name, (size, scale) in icons.items():
        with open(os.path.join(out, name), "wb") as f:
            f.write(png(size, render(size, scale)))
        print("wrote", name)


if __name__ == "__main__":
    main()
