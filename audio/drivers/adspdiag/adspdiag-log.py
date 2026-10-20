#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Read and decode the ADSP's DIAG stream collected by adspdiag.ko
# (/sys/kernel/debug/adspdiag/data). The kernel module does the control handshake.
# usage (root): adspdiag-log.py [seconds] [grep-regex]   (seconds 0 = forever)
#   RAW=1 also prints every raw chunk.
import os, sys, struct, select, time, re

DATA = '/sys/kernel/debug/adspdiag/data'
fd = os.open(DATA, os.O_RDONLY | os.O_NONBLOCK)
dur = float(sys.argv[1]) if len(sys.argv) > 1 else 0
flt = re.compile(sys.argv[2]) if len(sys.argv) > 2 else None
t_end = time.time() + dur if dur else None
buf = b''

def unhdlc(frame):
    out = bytearray(); esc = False
    for c in frame:
        if esc: out.append(c ^ 0x20); esc = False
        elif c == 0x7d: esc = True
        else: out.append(c)
    return bytes(out[:-2])  # drop CRC

def cstr(b, o):
    e = b.find(b'\0', o)
    if e < 0: e = len(b)
    return b[o:e].decode(errors='replace'), e + 1

def fmt_msg(fmt, args):
    out = []; ai = 0; i = 0
    while i < len(fmt):
        m = re.match(r'%[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z)?([diuxXcsp%])', fmt[i:])
        if not m: out.append(fmt[i]); i += 1; continue
        c = m.group(1); i += m.end()
        if c == '%': out.append('%'); continue
        v = args[ai] if ai < len(args) else 0; ai += 1
        if c in 'di': out.append(str(struct.unpack('<i', struct.pack('<I', v))[0]))
        elif c in 'xXp': out.append('%x' % v)
        elif c == 'c': out.append(chr(v & 0x7f))
        elif c == 's': out.append('<s:%x>' % v)
        else: out.append(str(v))
    return ''.join(out)

def handle(pkt):
    if not pkt: return
    cmd = pkt[0]
    if cmd == 0x79 and len(pkt) >= 20:  # DIAG_EXT_MSG_F
        ts_type, nargs, drop = pkt[1], pkt[2], pkt[3]
        ts, = struct.unpack_from('<Q', pkt, 4)
        line, ssid, mask = struct.unpack_from('<HHI', pkt, 12)
        args = list(struct.unpack_from('<%dI' % nargs, pkt, 20))
        fmt, o = cstr(pkt, 20 + 4 * nargs)
        fname, o = cstr(pkt, o)
        s = '%10.3f ssid=%d %s:%d %s' % ((ts >> 16) * 1.25 / 1000 / 1000 % 100000, ssid, fname, line, fmt_msg(fmt, args))
        if drop: s += ' [drop %d]' % drop
    else:
        s = 'pkt cmd=0x%02x len=%d %s' % (cmd, len(pkt), pkt[:48].hex())
    if flt is None or flt.search(s):
        print(s, flush=True)

while t_end is None or time.time() < t_end:
    try:
        chunk = os.read(fd, 65536)
    except BlockingIOError:
        time.sleep(0.05)
        continue
    if os.environ.get('RAW'): print('DATA', len(chunk), chunk[:64].hex(), flush=True)
    # the ADSP sends either non-HDLC (0x7e 0x01 len16 payload 0x7e) or HDLC frames
    if len(chunk) >= 4 and chunk[0] == 0x7e and chunk[1] == 0x01:
        o = 0
        while o + 4 <= len(chunk) and chunk[o] == 0x7e:
            l, = struct.unpack_from('<H', chunk, o + 2)
            handle(chunk[o+4:o+4+l]); o += 4 + l + 1
        continue
    buf += chunk
    while b'\x7e' in buf:
        fr, buf = buf.split(b'\x7e', 1)
        if fr: handle(unhdlc(fr))
