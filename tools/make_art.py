#!/usr/bin/env python3
"""Draw urlmagic4's artwork from the pixel art below (no image libraries needed):

  res/urlmagic4.ico   16x16 + 32x32, 16 colours (what NT 4.0 can show)
  docs/logo.png       README picture: the URL Magic tab, as IIS 4's Properties dialog draws it

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

# Small proportional pixel font in the spirit of MS Sans Serif 8: 9 rows (7 above the
# baseline, 2 for descenders), "X" = ink; each glyph is its rows, top down, separated by spaces.
GLYPHS = {
    "A": ".XXX. X...X X...X XXXXX X...X X...X X...X".split(),
    "C": ".XXX. X...X X.... X.... X.... X...X .XXX.".split(),
    "E": "XXXX X... X... XXX. X... X... XXXX".split(),
    "H": "X...X X...X X...X XXXXX X...X X...X X...X".split(),
    "L": "X... X... X... X... X... X... XXXX".split(),
    "M": "X...X XX.XX X.X.X X.X.X X...X X...X X...X".split(),
    "P": "XXXX. X...X X...X XXXX. X.... X.... X....".split(),
    "R": "XXXX. X...X X...X XXXX. X.X.. X..X. X...X".split(),
    "T": "XXXXX ..X.. ..X.. ..X.. ..X.. ..X.. ..X..".split(),
    "U": "X...X X...X X...X X...X X...X X...X .XXX.".split(),
    "4": "...X. ..XX. .X.X. X..X. XXXXX ...X. ...X.".split(),
    "a": "..... ..... .XXX. ....X .XXXX X...X .XXXX".split(),
    "b": "X.... X.... XXXX. X...X X...X X...X XXXX.".split(),
    "c": ".... .... .XXX X... X... X... .XXX".split(),
    "d": "....X ....X .XXXX X...X X...X X...X .XXXX".split(),
    "e": "..... ..... .XXX. X...X XXXXX X.... .XXX.".split(),
    "f": "..XX .X.. XXX. .X.. .X.. .X.. .X..".split(),
    "g": "..... ..... .XXXX X...X X...X X...X .XXXX ....X .XXX.".split(),
    "h": "X.... X.... XXXX. X...X X...X X...X X...X".split(),
    "i": "X . X X X X X".split(),
    "l": "X X X X X X X".split(),
    "m": "..... ..... XXXX. X.X.X X.X.X X.X.X X.X.X".split(),
    "n": "..... ..... XXXX. X...X X...X X...X X...X".split(),
    "o": "..... ..... .XXX. X...X X...X X...X .XXX.".split(),
    "r": ".... .... X.XX XX.. X... X... X...".split(),
    "s": ".... .... .XXX X... .XX. ...X XXX.".split(),
    "t": ".X.. .X.. XXX. .X.. .X.. .X.. ..XX".split(),
    "u": "..... ..... X...X X...X X...X X...X .XXXX".split(),
    "w": "..... ..... X...X X...X X.X.X X.X.X .X.X.".split(),
    " ": ".. .. .. .. .. .. ..".split(),
}

# Windows 95 check mark (7 x 7)
CHECK = ["......X", ".....XX", "X...XXX", "XX.XXX.", "XXXXX..", ".XXX...", "..X...."]


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


def text_width(text):
    return sum(len(GLYPHS[c][0]) + 1 for c in text) - 1


def draw_text(px, x, y, text, colour):
    """y = top of a capital letter"""
    for c in text:
        g = GLYPHS[c]
        for gy, row in enumerate(g):
            for gx, bit in enumerate(row):
                if bit == "X":
                    px[y + gy][x + gx] = colour
        x += len(g[0]) + 1


def banner():
    """Just the URL Magic tab, as IIS 4's Properties dialog draws a selected tab, at 1:1
    pixels, then scaled up."""
    grey, white, shadow, black = PALETTE[7], PALETTE[15], PALETTE[8], PALETTE[0]
    label = "URL Magic"
    tab_w = 6 + 16 + 3 + text_width(label) + 6
    margin = 4
    w, h = margin + tab_w + margin, 3 + 21 + 3
    px = [[grey] * w for _ in range(h)]
    x0, x1, top, base = margin, margin + tab_w - 1, 3, 3 + 20

    for x in range(x0 + 2, x1 - 1):
        px[top][x] = white                                       # top edge
    px[top + 1][x0 + 1] = white                                  # rounded corners
    px[top + 1][x1 - 1] = black
    for y in range(top + 2, base + 1):
        px[y][x0] = white                                        # left edge
        px[y][x1] = black                                        # right edge
        px[y][x1 - 1] = shadow
    px[top + 1][x1 - 2] = shadow
    for x in list(range(0, x0 + 1)) + list(range(x1, w)):       # the page's top edge
        px[base][x] = white

    for iy, row in enumerate(ICON):
        for ix, ch in enumerate(row):
            if ch != ".":
                px[top + 3 + iy][x0 + 6 + ix] = PALETTE[KEY[ch]]
    draw_text(px, x0 + 6 + 16 + 3, top + 8, label, black)

    f = 4                                                       # final pixel size
    return [[p for p in row for _ in range(f)] for row in px for _ in range(f)]


def main():
    assert len(ICON) == 16 and all(len(r) == 16 for r in ICON)
    write_ico("res/urlmagic4.ico")
    write_png("docs/logo.png", banner())


if __name__ == "__main__":
    main()
