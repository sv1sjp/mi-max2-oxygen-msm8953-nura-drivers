#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""diagdec.py CAPTURE [--summary] [--grep REGEX] [--ssid N]

Decode a raw ADSP DIAG capture (adspdiag-raw.py) from the oxygen ADSP.
- 0x79 extended messages carry their format string: printed directly.
- 0x92 QShrink messages carry {line, ssid, mask, hash} instead of a format string.
  The firmware holds the same 12 bytes as a constant struct; we find its address and
  the decompiled function that references it (work/out/adsp-annot.c), so every message
  maps to a code location even without its text.
"""
import argparse, bisect, os, re, struct, sys
from collections import Counter, OrderedDict

D = os.path.dirname(os.path.abspath(__file__))
# FW = folder with the ADSP firmware split files adsp.b00..b13 (bring your own)
FW = os.environ.get("FW", os.path.join(D, "fw"))
SEGS = {3: 0xf0102000, 4: 0xf015f000, 5: 0xf0821000, 6: 0xf0d35000, 7: 0xf0d36000,
        8: 0xf0000000, 9: 0xf0014000, 10: 0x2db00000, 11: 0x2dbf7000, 12: 0x2dcd3000,
        13: 0x2da00000}
DATA = {i: open(os.path.join(FW, "adsp.b%02d" % i), "rb").read() for i in SEGS}

def crc16(d):
    crc = 0xffff
    for b in d:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc ^ 0xffff

def frames(raw):
    for fr in raw.split(b"\x7e"):
        if len(fr) < 3:
            continue
        out, esc = bytearray(), False
        for c in fr:
            if esc:
                out.append(c ^ 0x20); esc = False
            elif c == 0x7d:
                esc = True
            else:
                out.append(c)
        pkt, crc = bytes(out[:-2]), struct.unpack("<H", bytes(out[-2:]))[0]
        yield pkt, crc16(pkt) == crc

_struct_cache = {}
def find_struct(line, ssid, mask, h):
    key = (line, ssid, mask, h)
    if key not in _struct_cache:
        pat = struct.pack("<HHII", line, ssid, mask, h)
        hits = []
        for i, base in SEGS.items():
            o = DATA[i].find(pat)
            while o >= 0:
                hits.append(base + o)
                o = DATA[i].find(pat, o + 1)
        _struct_cache[key] = hits
    return _struct_cache[key]

# Index: DAT_xxxxxxxx reference -> enclosing function (from the annotated Ghidra export).
_ref_index = None
def ref_funcs(addr):
    global _ref_index
    if _ref_index is None:
        _ref_index = {}
        cur = None
        with open(os.path.join(D, "work/out/adsp-annot.c"), encoding="latin1") as f:
            for L in f:
                m = re.match(r"^\S.*\b(FUN_[0-9a-f]{8})\s*\(", L)
                if m and not L.rstrip().endswith(";"):
                    cur = m.group(1)
                elif L.startswith("}"):
                    pass
                if cur and "DAT_" in L and not L.startswith("undefined"):
                    for a in re.findall(r"DAT_([0-9a-f]{8})", L):
                        _ref_index.setdefault(int(a, 16), set()).add(cur)
    r = sorted(_ref_index.get(addr, ()))
    return r or asm_funcs(addr)

# Fallback: ##0x constants in the raw llvm-objdump disassembly -> Ghidra function by entry.
_asm_index = None
def asm_funcs(addr):
    global _asm_index, _starts, _names
    if _asm_index is None:
        import pickle
        cache = os.path.join(D, "work/out/asmconst.pkl")
        if os.path.exists(cache):
            _asm_index = pickle.load(open(cache, "rb"))
        else:
            _asm_index = {}
            pat = re.compile(r"^([0-9a-f]{8}):.*##0x([0-9a-f]{8})")
            for L in open(os.path.join(FW, "a04.txt"), encoding="latin1"):
                m = pat.match(L)
                if m:
                    _asm_index.setdefault(int(m.group(2), 16), []).append(int(m.group(1), 16))
            pickle.dump(_asm_index, open(cache, "wb"))
        ents = sorted((int(l.split("\t")[0], 16), l.split("\t")[2].strip())
                      for l in open(os.path.join(D, "work/out/functions.tsv")))
        _starts = [e[0] for e in ents]
        _names = [e[1] for e in ents]
    out = set()
    for ia in _asm_index.get(addr, ()):
        i = bisect.bisect_right(_starts, ia) - 1
        if i >= 0:
            out.add("%s+%x" % (_names[i], ia - _starts[i]))
    return sorted(out)

def cstr(b, o):
    e = b.find(b"\0", o)
    e = len(b) if e < 0 else e
    return b[o:e].decode(errors="replace"), e + 1

def fmt(f, args):
    out, ai, i = [], 0, 0
    while i < len(f):
        m = re.match(r"%[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z)?([diuxXcsp%])", f[i:])
        if not m:
            out.append(f[i]); i += 1; continue
        c, i = m.group(1), i + m.end()
        if c == "%":
            out.append("%"); continue
        v = args[ai] if ai < len(args) else 0
        ai += 1
        out.append(str(struct.unpack("<i", struct.pack("<I", v))[0]) if c in "di"
                   else "%x" % v if c in "xXp" else chr(v & 0x7f) if c == "c"
                   else "<s:%x>" % v if c == "s" else str(v))
    return "".join(out)

def decode(raw):
    for pkt, ok in frames(raw):
        if not ok or not pkt:
            continue
        cmd = pkt[0]
        if cmd == 0x92 and len(pkt) >= 24:
            nargs = pkt[2]
            ts, = struct.unpack_from("<Q", pkt, 4)
            line, ssid, mask, h = struct.unpack_from("<HHII", pkt, 12)
            args = list(struct.unpack_from("<%dI" % nargs, pkt, 24)) if len(pkt) >= 24 + 4 * nargs else []
            yield dict(kind="q", ts=ts, line=line, ssid=ssid, mask=mask, hash=h, args=args)
        elif cmd == 0x79 and len(pkt) >= 20:
            nargs = pkt[2]
            ts, = struct.unpack_from("<Q", pkt, 4)
            line, ssid, mask = struct.unpack_from("<HHI", pkt, 12)
            args = list(struct.unpack_from("<%dI" % nargs, pkt, 20))
            f, o = cstr(pkt, 20 + 4 * nargs)
            fn, o = cstr(pkt, o)
            yield dict(kind="t", ts=ts, line=line, ssid=ssid, mask=mask, text=fmt(f, args), file=fn)
        else:
            yield dict(kind="other", cmd=cmd, hex=pkt[:32].hex())

def where(m):
    if m["kind"] != "q":
        return ""
    s = find_struct(m["line"], m["ssid"], m["mask"], m["hash"])
    if not s:
        return "?"
    fs = ref_funcs(s[0])
    return "%08x %s" % (s[0], ",".join(fs[:3]) if fs else "-")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--grep")
    ap.add_argument("--ssid", type=int)
    a = ap.parse_args()
    msgs = list(decode(open(a.capture, "rb").read()))
    t0 = next((m["ts"] for m in msgs if "ts" in m), 0)
    flt = re.compile(a.grep) if a.grep else None
    if a.summary:
        c = Counter()
        first = OrderedDict()
        for m in msgs:
            if m["kind"] == "other" or (a.ssid is not None and m["ssid"] != a.ssid):
                continue
            k = (m["ssid"], m["line"], m.get("hash"), m.get("text") if m["kind"] == "t" else None)
            c[k] += 1
            first.setdefault(k, m)
        for k, n in sorted(c.items(), key=lambda x: -x[1]):
            m = first[k]
            desc = m["text"][:90] if m["kind"] == "t" else "hash %08x args %s  @ %s" % (
                m["hash"], [hex(x) for x in m["args"][:5]], where(m))
            s = "%6d  ssid %5d line %5d  %s" % (n, k[0], k[1], desc)
            if not flt or flt.search(s):
                print(s)
        return
    for m in msgs:
        if m["kind"] == "other":
            s = "pkt cmd %#x %s" % (m["cmd"], m["hex"])
        else:
            if a.ssid is not None and m["ssid"] != a.ssid:
                continue
            t = ((m["ts"] - t0) >> 16) * 1.25 / 1e6  # ms
            body = m["text"] if m["kind"] == "t" else "hash %08x args %s @ %s" % (
                m["hash"], " ".join("%x" % x for x in m["args"]), where(m))
            s = "%10.3f ms ssid %5d L%-5d %s" % (t, m["ssid"], m["line"], body)
        if not flt or flt.search(s):
            print(s)

if __name__ == "__main__":
    main()
