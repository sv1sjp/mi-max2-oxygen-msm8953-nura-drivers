#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Send one HDLC-framed DIAG request to the ADSP through adspdiag.ko and print the raw
# reply bytes for a few seconds.  usage (root): adspdiag-req.py HEXBYTES [seconds]
import os, sys, struct, time

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
        out += bytes((0x7d, b ^ 0x20)) if b in (0x7e, 0x7d) else bytes((b,))
    return bytes(out) + b'\x7e'

DATA = '/sys/kernel/debug/adspdiag/data'
req = bytes.fromhex(sys.argv[1])
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 3
rfd = os.open(DATA, os.O_RDONLY | os.O_NONBLOCK)
wfd = os.open(DATA, os.O_WRONLY)
os.write(wfd, hdlc(req))
print('sent', hdlc(req).hex(), flush=True)
end = time.time() + dur
while time.time() < end:
    try:
        c = os.read(rfd, 65536)
        print('rx', len(c), c[:96].hex(), flush=True)
    except BlockingIOError:
        time.sleep(0.05)
