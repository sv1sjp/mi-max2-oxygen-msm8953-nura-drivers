#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Diff LPASS-CC snapshots (regsnap.ko dumps of 0x0c000000..+0x14000).

usage: diff.py base.bin other.bin [more.bin ...]
Prints every word that differs between any of the snapshots, and flags words
that look like Qualcomm branch control registers (CBCR: bit0 CLK_ENABLE,
bit31 CLK_OFF) - "EN but OFF" means enabled but the clock is not running.
"""
import struct
import sys

BASE = 0x0c000000
files = sys.argv[1:]
snaps = [open(f, 'rb').read() for f in files]
n = min(len(s) for s in snaps) // 4
words = [struct.unpack('<%dI' % n, s[:n * 4]) for s in snaps]
names = [f.rsplit('/', 1)[-1].replace('lpasscc-', '').replace('.bin', '') for f in files]


def cbcr(v):
    notes = []
    if v & 1:
        notes.append('EN')
    if v & 0x80000000:
        notes.append('OFF')
    return '+'.join(notes)


print('%-10s ' % 'addr' + ' '.join('%-14s' % x for x in names))
for i in range(n):
    vals = [w[i] for w in words]
    if len(set(vals)) > 1:
        line = '0x%08x ' % (BASE + i * 4)
        line += ' '.join('%08x %-5s' % (v, cbcr(v)) for v in vals)
        print(line)
