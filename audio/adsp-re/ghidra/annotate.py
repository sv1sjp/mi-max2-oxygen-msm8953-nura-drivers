#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""annotate.py : work/out/adsp.c -> work/out/adsp-annot.c, appending the string / diag
message text behind every DAT_xxxxxxxx / s_ reference as a trailing comment."""
import os, re, struct
D = os.path.dirname(os.path.abspath(__file__))
# FW = folder with the ADSP firmware split files adsp.b00..b13 (bring your own)
FW = os.environ.get("FW", os.path.join(D, "fw"))
segs = {3: 0xf0102000, 4: 0xf015f000, 5: 0xf0821000, 6: 0xf0d35000, 7: 0xf0d36000, 8: 0xf0000000,
        9: 0xf0014000, 10: 0x2db00000, 11: 0x2dbf7000, 12: 0x2dcd3000, 13: 0x2da00000}
data = {i: open(os.path.join(FW, "adsp.b%02d" % i), "rb").read() for i in segs}

def rd(a, n=4):
    for i, b in segs.items():
        if b <= a and a + n <= b + len(data[i]):
            return data[i][a - b:a - b + n]

def cstr(a):
    for i, b in segs.items():
        if b <= a < b + len(data[i]):
            d, o = data[i], a - b
            e = d.find(b"\0", o, o + 200)
            if e > o + 3 and all(32 <= c < 127 or c in (9, 10) for c in d[o:e]):
                return d[o:e].decode()

cache = {}
def desc(a):
    if a not in cache:
        t = cstr(a)
        if t:
            cache[a] = '"%s"' % t[:100]
        else:
            r = rd(a, 12)
            t = r and cstr(struct.unpack("<3I", r)[2])
            cache[a] = 'MSG "%s"' % t[:120] if t else None
    return cache[a]

out = open(os.path.join(D, "work/out/adsp-annot.c"), "w")
for L in open(os.path.join(D, "work/out/adsp.c"), encoding="latin1"):
    L = L.rstrip("\n")
    n = [desc(int(x, 16)) for x in re.findall(r"(?:DAT|PTR_DAT|s_\w*?)_([0-9a-f]{8})\b", L)]
    n = [x for x in n if x]
    out.write(L + ("  /* " + " | ".join(n).replace("*/", "*_/") + " */" if n else "") + "\n")
