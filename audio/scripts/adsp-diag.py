#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Minimal diag client for the ADSP: enables all F3 debug messages over DIAG_CNTL
# and prints them from the DIAG channel. Needs rpmsg_char bound to the ADSP's
# DIAG + DIAG_CNTL channels (scripts/adsp-diag-bind.sh).
# usage: adsp-diag.py [seconds] [grep-regex]   (seconds 0 = forever)
import os, sys, struct, select, time, re

def find(name):
    for d in os.listdir('/sys/class/rpmsg'):
        try:
            if open('/sys/class/rpmsg/%s/name' % d).read().strip() == name:
                return '/dev/' + d
        except OSError:
            pass
    sys.exit('no rpmsg dev for ' + name)

data = os.open(find('DIAG'), os.O_RDWR)
cntl = os.open(find('DIAG_CNTL'), os.O_RDWR)

def cntl_send(cmd, payload):
    os.write(cntl, struct.pack('<II', cmd, len(payload)) + payload)

sent_masks = False
def send_masks():
  # feature mask (cmd 8): advertise nothing special -> peripheral keeps HDLC on its side
  cntl_send(8, struct.pack('<I', 4) + b'\x00\x00\x00\x00')
  # diag mode (cmd 3): real-time, no sleep vote
  cntl_send(3, struct.pack('<IIIIIIIII', 1, 1, 1, 0, 0, 0, 0, 0, 0))
  # F3 mask (cmd 11): stream 1, status 2 = ALL_ENABLED, msg_mode 0, ssid 0..0, range_len 0
  cntl_send(11, struct.pack('<BBBHHI', 1, 2, 0, 0, 0, 0))
send_masks()

def crc16(d):
    crc = 0xffff
    for b in d:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc ^ 0xffff

def hdlc(d):
    d = d + struct.pack('<H', crc16(d))
    out = bytearray()
    for b in d:
        if b in (0x7e, 0x7d): out += bytes((0x7d, b ^ 0x20))
        else: out.append(b)
    return bytes(out) + b'\x7e'

# DIAG_EXT_MSG_CONFIG_F (0x7d) sub 5 = set all RT masks
os.write(data, hdlc(struct.pack('<BBBBI', 0x7d, 5, 0, 0, 0xffffffff)))

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

while True:
    to = None if t_end is None else max(0, t_end - time.time())
    if t_end is not None and to == 0: break
    r, _, _ = select.select([data, cntl], [], [], to)
    for fd in r:
        chunk = os.read(fd, 65536)
        if fd == cntl:
            o = 0
            while o + 8 <= len(chunk):
                c, l = struct.unpack_from('<II', chunk, o)
                if c == 8 and not sent_masks: send_masks(); sent_masks = True
                if c == 25 and l >= 12:
                    first, last = struct.unpack_from('<HH', chunk, o + 16)
                    n = last - first + 1
                    cntl_send(11, struct.pack('<BBBHHI', 1, 3, 0, first, last, n) + b'\xff\xff\xff\xff' * n)
                if flt is None and os.environ.get('CNTL'): print('CNTL cmd=%d len=%d %s' % (c, l, chunk[o+8:o+8+min(l, 40)].hex()), flush=True)
                o += 8 + l
            continue
        if os.environ.get('RAW'): print('DATA', len(chunk), chunk[:64].hex(), flush=True)
        # data: peripheral may send raw (non-HDLC) with 0x7e 0x01 len header, or HDLC frames
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
