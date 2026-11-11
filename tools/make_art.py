#!/usr/bin/env python3
"""Draw urlmagic4's artwork from the pixel art below (no image libraries needed):

  res/urlmagic4.ico   16x16 + 32x32, 16 colours (what NT 4.0 can show)
  docs/logo.png       README picture: the URL Magic tab as IIS 4's Properties dialog shows it

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
# baseline, 2 for descenders), "X" = ink.
GLYPHS = {
    "A": [".XXX.", "X...X", "X...X", "XXXXX", "X...X", "X...X", "X...X"],
    "C": [".XXX.", "X...X", "X....", "X....", "X....", "X...X", ".XXX."],
    "E": ["XXXX", "X...", "X...", "XXX.", "X...", "X...", "XXXX"],
    "H": ["X...X", "X...X", "X...X", "XXXXX", "X...X", "X...X", "X...X"],
    "L": ["X...", "X...", "X...", "X...", "X...", "X...", "XXXX"],
    "M": ["X...X", "XX.XX", "X.X.X", "X.X.X", "X...X", "X...X", "X...X"],
    "P": ["XXXX.", "X...X", "X...X", "XXXX.", "X....", "X....", "X...."],
    "R": ["XXXX.", "X...X", "X...X", "XXXX.", "X.X..", "X..X.", "X...X"],
    "T": ["XXXXX", "..X..", "..X..", "..X..", "..X..", "..X..", "..X.."],
    "U": ["X...X", "X...X", "X...X", "X...X", "X...X", "X...X", ".XXX."],
    "4": ["...X.", "..XX.", ".X.X.", "X..X.", "XXXXX", "...X.", "...X."],
    "a": [".....", ".....", ".XXX.", "....X", ".XXXX", "X...X", ".XXXX"],
    "b": ["X....", "X....", "XXXX.", "X...X", "X...X", "X...X", "XXXX."],
    "c": ["....", "....", ".XXX", "X...", "X...", "X...", ".XXX"],
    "d": ["....X", "....X", ".XXXX", "X...X", "X...X", "X...X", ".XXXX"],
    "e": [".....", ".....", ".XXX.", "X...X", "XXXXX", "X....", ".XXX."],
    "f": ["..XX", ".X..", "XXX.", ".X..", ".X..", ".X..", ".X.."],
    "g": [".....", ".....", ".XXXX", "X...X", "X...X", "X...X", ".XXXX", "....X", ".XXX."],
    "h": ["X....", "X....", "XXXX.", "X...X", "X...X", "X...X", "X...X"],
    "i": ["X", ".", "X", "X", "X", "X", "X"],
    "l": ["X", "X", "X", "X", "X", "X", "X"],
    "m": [".....", ".....", "XXXX.", "X.X.X", "X.X.X", "X.X.X", "X.X.X"],
    "n": [".....", ".....", "XXXX.", "X...X", "X...X", "X...X", "X...X"],
    "o": [".....", ".....", ".XXX.", "X...X", "X...X", "X...X", ".XXX."],
    "r": ["....", "....", "X.XX", "XX..", "X...", "X...", "X..."],
    "s": ["....", "....", ".XXX", "X...", ".XX.", "...X", "XXX."],
    "t": [".X..", ".X..", "XXX.", ".X..", ".X..", ".X..", "..XX"],
    "u": [".....", ".....", "X...X", "X...X", "X...X", "X...X", ".XXXX"],
    "w": [".....", ".....", "X...X", "X...X", "X.X.X", "X.X.X", ".X.X."],
    " ": ["..", "..", "..", "..", "..", "..", ".."],
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
    """The URL Magic tab as IIS 4's site Properties dialog shows it, at 1:1 pixels, then
    scaled up. Neighbouring tabs for context; the page shows the tab's first control."""
    grey, white, shadow, black = PALETTE[7], PALETTE[15], PALETTE[8], PALETTE[0]
    tabs = ["HTTP Headers", "Custom Errors", "URL Magic"]      # the last one is selected
    label = "Enable URL rewriting for this site"

    widths = [text_width(t) + 12 for t in tabs]
    widths[-1] += 16 + 3                                         # icon + gap
    page_top = 22
    w = max(4 + sum(widths) + 40, 4 + 12 + 13 + 4 + text_width(label) + 16)
    h = page_top + 32
    px = [[grey] * w for _ in range(h)]

    def hline(y, x0, x1, c):
        for x in range(x0, x1 + 1):
            px[y][x] = c

    def vline(x, y0, y1, c):
        for y in range(y0, y1 + 1):
            px[y][x] = c

    # the page: raised panel
    left, right, bottom = 2, w - 3, h - 3
    hline(page_top, left, right, white)
    vline(left, page_top, bottom, white)
    hline(bottom, left, right, black)
    hline(bottom - 1, left + 1, right - 1, shadow)
    vline(right, page_top, bottom, black)
    vline(right - 1, page_top + 1, bottom - 1, shadow)

    # the tabs
    x = left
    for i, (t, tw) in enumerate(zip(tabs, widths)):
        selected = i == len(tabs) - 1
        x0, x1 = (x - 2, x + tw + 1) if selected else (x, x + tw - 1)
        top = page_top - 19 if selected else page_top - 17
        if selected:
            hline(page_top, x0 + 1, x1 - 1, grey)               # the tab joins the page
            for y in range(top + 1, page_top + 1):
                hline(y, x0 + 1, x1 - 2, grey)
        hline(top, x0 + 2, x1 - 2, white)
        px[top + 1][x0 + 1] = white
        vline(x0, top + 2, page_top - (0 if selected else 1), white)
        px[top + 1][x1 - 1] = black
        vline(x1, top + 2, page_top - (0 if selected else 1), black)
        vline(x1 - 1, top + 1, page_top - (0 if selected else 1), shadow)
        tx = x0 + 6 + (2 if selected else 0)
        ty = top + 5
        if selected:
            for iy, row in enumerate(ICON):
                for ix, ch in enumerate(row):
                    if ch != ".":
                        px[top + 2 + iy][tx + ix] = PALETTE[KEY[ch]]
            tx += 16 + 3
            ty = top + 7
        draw_text(px, tx, ty, t, black)
        x += tw

    # first control on the page: a ticked checkbox
    cx, cy = left + 10, page_top + 10
    hline(cy, cx, cx + 12, shadow)
    vline(cx, cy, cy + 12, shadow)
    hline(cy + 1, cx + 1, cx + 11, black)
    vline(cx + 1, cy + 1, cy + 11, black)
    hline(cy + 12, cx, cx + 12, white)
    vline(cx + 12, cy, cy + 12, white)
    hline(cy + 11, cx + 1, cx + 11, grey)
    vline(cx + 11, cy + 1, cy + 11, grey)
    for y in range(cy + 2, cy + 11):
        hline(y, cx + 2, cx + 10, white)
    for gy, row in enumerate(CHECK):
        for gx, bit in enumerate(row):
            if bit == "X":
                px[cy + 3 + gy][cx + 3 + gx] = black
    draw_text(px, cx + 13 + 4, cy + 3, label, black)

    f = 3                                                       # final pixel size
    return [[p for p in row for _ in range(f)] for row in px for _ in range(f)]


def main():
    assert len(ICON) == 16 and all(len(r) == 16 for r in ICON)
    write_ico("res/urlmagic4.ico")
    write_png("docs/logo.png", banner())


if __name__ == "__main__":
    main()
