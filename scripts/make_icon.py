"""Generates res/app.ico (the ring-gauge app icon) with only the standard library."""
import math
import os
import struct
import zlib

CLAY = (217, 119, 87)
TRACK = (140, 140, 140)
FILL = 0.68


def render(size):
    c = size / 2.0
    r_out = size / 2.0 - 0.5
    r_in = r_out * 0.52
    ss = 4
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            r = g = b = a = 0.0
            for sy in range(ss):
                for sx in range(ss):
                    px = x + (sx + 0.5) / ss - c
                    py = y + (sy + 0.5) / ss - c
                    d = math.hypot(px, py)
                    if d > r_out or d < r_in:
                        continue
                    ang = math.atan2(px, -py)
                    if ang < 0:
                        ang += 2 * math.pi
                    on = ang / (2 * math.pi) < FILL
                    col, sa = (CLAY, 1.0) if on else (TRACK, 0.55)
                    r += col[0] * sa
                    g += col[1] * sa
                    b += col[2] * sa
                    a += sa
            n = ss * ss
            if a:
                row += bytes((round(r / a), round(g / a), round(b / a), round(a / n * 255)))
            else:
                row += b"\0\0\0\0"
        rows.append(bytes(row))
    return rows


def png(size, rows):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    raw = b"".join(b"\0" + r for r in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    sizes = [16, 20, 24, 32, 40, 48, 64, 256]
    images = [png(s, render(s)) for s in sizes]
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b""
    for s, data in zip(sizes, images):
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "res", "app.ico")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(header + entries + b"".join(images))
    print("wrote", out)


if __name__ == "__main__":
    main()
