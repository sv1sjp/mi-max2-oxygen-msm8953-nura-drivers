#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# (Re)send the DIAG control setup to the ADSP through adspdiag.ko's cntl file:
# diag mode real-time, STM off, F3 masks (cmd 11 + cmd 16 presets 1-2) for all SSID ranges.
import os, struct
R = [(0, 111), (500, 506), (1000, 1007), (2000, 2008), (3000, 3014), (4000, 4010),
     (4500, 4526), (4600, 4615), (5000, 5030), (5500, 5516), (6000, 6080), (6500, 6521),
     (7000, 7003), (7100, 7111), (7200, 7201), (8000, 8000), (8500, 8524), (9000, 9008),
     (9500, 9510), (10200, 10210), (10251, 10255), (10300, 10300), (10350, 10374)]
fd = os.open('/sys/kernel/debug/adspdiag/cntl', os.O_WRONLY)
def send(cmd, payload):
    os.write(fd, struct.pack('<II', cmd, len(payload)) + payload)
send(8, struct.pack("<I", 4) + bytes([1, 0, 0, 0]))  # byte 0 != 0, else the ADSP drops all output
send(3, struct.pack('<9I', 1, 1, 1, 0, 0, 0, 0, 0, 0))
send(21, struct.pack('<IB', 1, 0))
n = 0
for f, l in R:
    k = l - f + 1
    send(11, struct.pack('<BBBHHI', 1, 3, 0, f, l, k) + b'\xff' * 4 * k); n += 1
    for p in (1, 2):
        send(16, struct.pack('<BBBBHHI', 1, p, 3, 0, f, l, k) + b'\xff' * 4 * k); n += 1
print('sent', n, 'mask packets')
