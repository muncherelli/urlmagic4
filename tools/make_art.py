#!/usr/bin/env python3
"""Draw urlmagic4's artwork from the pixel art below (no image libraries needed):

  res/urlmagic4.ico   16x16 + 32x32, 16 colours (what NT 4.0 can show)
  docs/logo.png       README banner: the icon and "URL MAGIC 4" on a Windows 95-style panel

Run from the repo root:  python3 tools/make_art.py
"""
import struct
import zlib

# Windows' standard 16-colour palette
PALETTE = [(0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0), (0, 0, 128), (128, 0, 128),
           (0, 128, 128), (192, 192, 192), (128, 128, 128), (255, 0, 0), (0, 255, 0),
           (255, 255, 0), (0, 0, 255), (255, 0, 255), (0, 255, 255), (255, 255, 255)]
# pixel art key -> palette index; '.' = transparent
KEY = {"K": 0, "D": 1, "N": 4, "G": 8, "S": 7, "R": 9, "Y": 11, "B": 12, "M": 13, "C": 14, "W": 15}

# A magic wand with sparkles and a red 4 (16 x 16)
ICON = [
    "..........Y.....",
    "..........Y.....",
    "........YYWYY...",
    "..........Y..M..",
    "....C.....Y.....",
    "...CWC..WW......",
    "....C..WWWK...Y.",
    ".......WWKK..YWY",
    "......KKKK....Y.",
    ".....KKKK....R..",
    "....KKKK....RR..",
    "...KKKK....R.R..",
    "..KKKK....R..R..",
    ".KKKK.....RRRRR.",
    ".KKK.........R..",
    "..K..........R..",
]

# 5 x 7 pixel font for the banner title
FONT = {
    "U": ["X...X", "X...X", "X...X", "X...X", "X...X", "X...X", ".XXX."],
    "R": ["XXXX.", "X...X", "X...X", "XXXX.", "X.X..", "X..X.", "X...X"],
    "L": ["X....", "X....", "X....", "X....", "X....", "X....", "XXXXX"],
    "M": ["X...X", "XX.XX", "X.X.X", "X.X.X", "X...X", "X...X", "X...X"],
    "A": [".XXX.", "X...X", "X...X", "XXXXX", "X...X", "X...X", "X...X"],
    "G": [".XXX.", "X...X", "X....", "X.XXX", "X...X", "X...X", ".XXXX"],
    "I": [".XXX.", "..X..", "..X..", "..X..", "..X..", "..X..", ".XXX."],
    "C": [".XXX.", "X...X", "X....", "X....", "X....", "X...X", ".XXX."],
    "4": ["...X.", "..XX.", ".X.X.", "X..X.", "XXXXX", "...X.", "...X."],
    " ": ["...", "...", "...", "...", "...", "...", "..."],
}


def scale(art, f):
    return ["".join(ch * f for ch in row) for row in art for _ in range(f)]


# ---------------------------------------------------------------- .ico

def ico_image(art):
    """One ICO image (BITMAPINFOHEADER + palette + 4bpp XOR + 1bpp AND mask), bottom-up."""
    n = len(art)
    xor_row = ((n * 4 + 31) // 32) * 4
    and_row = ((n + 31) // 32) * 4
    xor = bytearray()
    mask = bytearray()
    for row in reversed(art):
        bits = bytearray(xor_row)
        m = bytearray(and_row)
        for x, ch in enumerate(row):
            bits[x // 2] |= KEY.get(ch, 0) << (4 if x % 2 == 0 else 0)
            if ch == ".":
                m[x // 8] |= 0x80 >> (x % 8)
        xor += bits
        mask += m
    header = struct.pack("<IiiHHIIiiII", 40, n, n * 2, 1, 4, 0, len(xor) + len(mask), 0, 0, 16, 0)
    pal = b"".join(struct.pack("<BBBB", b, g, r, 0) for r, g, b in PALETTE)
    return header + pal + bytes(xor) + bytes(mask)


def write_ico(path):
    images = [(16, ico_image(ICON)), (32, ico_image(scale(ICON, 2)))]
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for size, data in images:
        out += struct.pack("<BBBBHHII", size, size, 16, 0, 1, 4, len(data), offset)
        offset += len(data)
    with open(path, "wb") as f:
        f.write(out + b"".join(d for _, d in images))


# ---------------------------------------------------------------- .png

def write_png(path, pixels):
    """pixels: rows of (r, g, b)"""
    h, w = len(pixels), len(pixels[0])
    raw = b"".join(b"\0" + b"".join(bytes(p) for p in row) for row in pixels)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def banner():
    """Canvas in 'art pixels', scaled up at the end."""
    grey, white, shadow, dark, navy, red = (PALETTE[7], PALETTE[15], PALETTE[8], PALETTE[0],
                                            PALETTE[4], PALETTE[9])
    icon = scale(ICON, 2)                      # 32 x 32
    title = "URL MAGIC 4"
    tscale = 2
    glyphs = [FONT[c] for c in title]
    text_w = sum((len(g[0]) + 1) * tscale for g in glyphs) - tscale
    pad, gap = 6, 8
    w = pad + len(icon[0]) + gap + text_w + pad
    h = pad + len(icon) + pad
    px = [[grey] * w for _ in range(h)]

    # Windows 95 raised panel: white top/left, dark bottom/right
    for x in range(w):
        px[0][x] = white
        px[h - 1][x] = dark
        px[h - 2][x] = shadow if 0 < x < w - 1 else px[h - 2][x]
    for y in range(h):
        px[y][0] = white
        px[y][w - 1] = dark
        if 0 < y < h - 1:
            px[y][w - 2] = shadow

    for y, row in enumerate(icon):
        for x, ch in enumerate(row):
            if ch != ".":
                px[pad + y][pad + x] = PALETTE[KEY[ch]]

    x0 = pad + len(icon[0]) + gap
    y0 = (h - 7 * tscale) // 2
    for c, g in zip(title, glyphs):
        colour = red if c == "4" else navy
        for gy, grow in enumerate(g):
            for gx, bit in enumerate(grow):
                if bit == "X":
                    for dy in range(tscale):
                        for dx in range(tscale):
                            px[y0 + gy * tscale + dy][x0 + gx * tscale + dx] = colour
        x0 += (len(g[0]) + 1) * tscale

    f = 4                                      # final pixel size
    return [[p for p in row for _ in range(f)] for row in px for _ in range(f)]


def main():
    assert len(ICON) == 16 and all(len(r) == 16 for r in ICON)
    write_ico("res/urlmagic4.ico")
    write_png("docs/logo.png", banner())


if __name__ == "__main__":
    main()
