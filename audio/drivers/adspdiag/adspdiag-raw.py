#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Save the raw ADSP DIAG stream from adspdiag.ko to a file. usage (root): adspdiag-raw.py OUT [seconds]
import os, sys, time
fd = os.open('/sys/kernel/debug/adspdiag/data', os.O_RDONLY | os.O_NONBLOCK)
out = open(sys.argv[1], 'wb')
end = time.time() + (float(sys.argv[2]) if len(sys.argv) > 2 else 10)
while time.time() < end:
    try:
        out.write(os.read(fd, 65536)); out.flush()
    except BlockingIOError:
        time.sleep(0.02)
