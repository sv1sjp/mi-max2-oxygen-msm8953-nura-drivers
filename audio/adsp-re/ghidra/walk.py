#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""walk.py ROOT DEPTH : call tree from ROOT; flags loops (L), first diag messages."""
import re, sys
argv = sys.argv[1:]
sys.argv = ["fn.py"]
exec(open(__file__.replace("walk.py", "fn.py")).read().split("args = sys.argv[1:]")[0])
def callees(n):
    b = body(n) or ""
    return list(dict.fromkeys(x.replace("thunk_", "") for x in
                re.findall(r"\b((?:thunk_)?FUN_[0-9a-f]{8})\(", b) if x != n))
skip = set(argv[2:])
seen = set()
def walk(n, d, maxd):
    if n in seen or d > maxd or n in skip:
        return
    seen.add(n)
    b = body(n) or ""
    loops = len(re.findall(r"\bwhile\s*\(|\bdo\s*\{", b))
    msgs = re.findall(r'MSG "([^"]{0,70})', b)[:2]
    print("  " * d + n, len(b), ("L%d" % loops) if loops else "", " | ".join(msgs))
    for c in callees(n):
        walk(c, d + 1, maxd)
walk(argv[0], 0, int(argv[1]))
