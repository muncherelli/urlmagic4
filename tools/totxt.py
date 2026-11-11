#!/usr/bin/env python3
"""Turn a Markdown/UTF-8 doc into a .txt that NT 4.0's Notepad reads: Windows-1252, CRLF.
   python3 tools/totxt.py IN OUT"""
import sys

ASCII = {"→": "->", "←": "<-", "—": "--", "–": "-", "…": "...",
         "‘": "'", "’": "'", "“": '"', "”": '"', " ": " ",
         "•": "*", "·": "*", "™": "(TM)", "≥": ">=", "≤": "<="}

text = open(sys.argv[1], encoding="utf-8").read().replace("\r\n", "\n")
for u, a in ASCII.items():
    text = text.replace(u, a)
with open(sys.argv[2], "wb") as f:
    f.write(text.replace("\n", "\r\n").encode("cp1252", errors="replace"))
