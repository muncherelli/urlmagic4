#!/usr/bin/env python3
"""Write res/urlmagic4.ico (16x16 + 32x32, 16 colours: what NT 4.0 can show) from the pixel
art below.  Run from the repo root:  python3 tools/make_icon.py [preview.png]"""
import struct
import sys
import zlib

# Windows' standard 16-colour palette
PALETTE = [(0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0), (0, 0, 128), (128, 0, 128),
           (0, 128, 128), (192, 192, 192), (128, 128, 128), (255, 0, 0), (0, 255, 0),
           (255, 255, 0), (0, 0, 255), (255, 0, 255), (0, 255, 255), (255, 255, 255)]
# pixel art key -> palette index; '.' = transparent
KEY = {"K": 0, "N": 4, "B": 12, "W": 15, "S": 7, "Y": 11, "O": 3, "M": 13, "C": 14}

# A magic wand with sparkles (16 x 16)
ART = [
    "..........Y.....",
    "..........Y.....",
    "........YYWYY...",
    "..........Y..M..",
    "....C.....Y.....",
    "...CWC..WW......",
    "....C..WWWK...Y.",
    ".......WWKK..YWY",
    "......KKKK....Y.",
    ".....KKKK.......",
    "....KKKK....M...",
    "...KKKK.........",
    "..KKKK..........",
    ".KKKK...........",
    ".KKK............",
    "..K.............",
]


def bitmap(art):
    """One ICO image (BITMAPINFOHEADER + palette + 4bpp XOR + 1bpp AND), rows bottom-up."""
    n = len(art)
    xor_row = ((n * 4 + 31) // 32) * 4
    and_row = ((n + 31) // 32) * 4
    xor = bytearray()
    mask = bytearray()
    for row in reversed(art):
        bits = bytearray(xor_row)
        m = bytearray(and_row)
        for x, ch in enumerate(row):
            idx = KEY.get(ch, 0)
            bits[x // 2] |= idx << (4 if x % 2 == 0 else 0)
            if ch == ".":
                m[x // 8] |= 0x80 >> (x % 8)
        xor += bits
        mask += m
    header = struct.pack("<IiiHHIIiiII", 40, n, n * 2, 1, 4, 0, len(xor) + len(mask), 0, 0, 16, 0)
    pal = b"".join(struct.pack("<BBBB", b, g, r, 0) for r, g, b in PALETTE)
    return header + pal + bytes(xor) + bytes(mask)


def scale(art, f):
    return ["".join(ch * f for ch in row) for row in art for _ in range(f)]


def png(art, path, bg=(212, 208, 200)):
    """Preview on the NT4 dialog grey."""
    n = len(art)
    raw = b"".join(b"\0" + b"".join(bytes(PALETTE[KEY[c]] if c != "." else bg) for c in row)
                   for row in art)
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def main():
    assert len(ART) == 16 and all(len(r) == 16 for r in ART)
    images = [(16, bitmap(ART)), (32, bitmap(scale(ART, 2)))]
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for size, data in images:
        out += struct.pack("<BBBBHHII", size, size, 16, 0, 1, 4, len(data), offset)
        offset += len(data)
    out += b"".join(d for _, d in images)
    with open("res/urlmagic4.ico", "wb") as f:
        f.write(out)
    if len(sys.argv) > 1:
        png(scale(ART, 8), sys.argv[1])


if __name__ == "__main__":
    main()
