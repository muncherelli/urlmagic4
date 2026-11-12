#!/usr/bin/env python3
"""Turn a Markdown/UTF-8 doc into a .txt that NT 4.0's Notepad reads: Windows-1252, CRLF.
   Lines that are only HTML (the README's logo) are left out.
   python3 tools/totxt.py IN OUT"""
import re
import sys

ASCII = {"→": "->", "←": "<-", "—": "--", "–": "-", "…": "...",
         "‘": "'", "’": "'", "“": '"', "”": '"', " ": " ",
         "•": "*", "·": "*", "™": "(TM)", "≥": ">=", "≤": "<="}

# a build step: the Makefile names both files
text = open(sys.argv[1], encoding="utf-8").read().replace("\r\n", "\n")  # NOSONAR
text = "\n".join(l for l in text.split("\n") if not re.fullmatch(r"\s*<.*>\s*", l))
text = text.lstrip("\n")
for u, a in ASCII.items():
    text = text.replace(u, a)
with open(sys.argv[2], "wb") as f:  # NOSONAR
    f.write(text.replace("\n", "\r\n").encode("cp1252", errors="replace"))
