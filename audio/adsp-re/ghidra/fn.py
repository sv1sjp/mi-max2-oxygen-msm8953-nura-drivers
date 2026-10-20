#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""fn.py ADDR|NAME [...]  print Ghidra-decompiled C of the function containing ADDR
(or named NAME) from work/out/adsp.c.  fn.py -x ADDR  lists functions that call it."""
import bisect, os, re, sys
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "work/out")
src = open(os.path.join(OUT, "adsp-annot.c" if os.path.exists(os.path.join(OUT, "adsp-annot.c")) else "adsp.c"), encoding="latin1").read()
ents = []
for l in open(os.path.join(OUT, "functions.tsv")):
    a, sz, n = l.rstrip("\n").split("\t")
    ents.append((int(a, 16), int(sz), n))
ents.sort()
starts = [e[0] for e in ents]
defs = {m.group(1): m.start() for m in
        re.finditer(r"\n[^\n;]*?\b(\w+)\s*\([^;{]*\)\s*\n\{", src)}

def body(name):
    i = defs.get(name)
    if i is None:
        return None
    k, d = src.index("{", i), 0
    while True:
        c = src[k]
        d += c == "{"
        d -= c == "}"
        if c == "}" and d == 0:
            return src[i:k + 1]
        k += 1

def lookup(q):
    if re.fullmatch(r"(0x)?[0-9a-fA-F]{8}", q):
        a = int(q, 16)
        i = bisect.bisect_right(starts, a) - 1
        return ents[i] if i >= 0 else None
    return next((e for e in ents if e[2] == q), None)

args = sys.argv[1:]
if args and args[0] == "-x":
    for q in args[1:]:
        e = lookup(q)
        callers = sorted({n for n, i in defs.items()
                          if n != e[2] and e[2] in (body(n) or "")})
        print(e[2], "callers:", " ".join(callers))
    sys.exit()
for q in args:
    e = lookup(q)
    print("== %s (entry %08x, %d bytes)" % (e[2], e[0], e[1]))
    print(body(e[2]) or "(no body in export)")
