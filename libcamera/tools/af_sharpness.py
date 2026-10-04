#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Sharpness of RAW10 CSI2P crops (4032 wide): variance of Laplacian on green.

usage: af_sharpness.py <dir with af-<pos>.crop files from sweep.sh>
Prints a score per lens position and the best one; use it to check the
infinity/macro range of the Af block in imx386.yaml.
"""
import sys, glob, re, numpy as np
W = 4032
def load(path):
    b = np.fromfile(path, np.uint8)
    h = len(b) // (W * 10 // 8)
    b = b[:h * W * 10 // 8].reshape(h, W // 4, 5).astype(np.uint16)
    px = np.empty((h, W // 4, 4), np.uint16)
    for i in range(4):
        px[:, :, i] = (b[:, :, i] << 2) | ((b[:, :, 4] >> (2 * i)) & 3)
    return px.reshape(h, W).astype(np.float32)
res = []
for f in glob.glob(sys.argv[1] + '/af-*.crop'):
    pos = int(re.search(r'af-(\d+)\.crop$', f).group(1))
    g = load(f)[0::2, 1::2]            # Gr plane
    g = g[:, 500:1500]                 # central area
    lap = g[1:-1, 1:-1] * 4 - g[:-2, 1:-1] - g[2:, 1:-1] - g[1:-1, :-2] - g[1:-1, 2:]
    res.append((pos, lap.var() / max(g.mean() - 64, 1) ** 2 * 1e4, g.mean()))
best = max(res, key=lambda r: r[1])
for pos, s, m in sorted(res):
    print('%5d  sharp %8.2f  mean %6.1f %s' % (pos, s, m, '#' * int(40 * s / best[1])))
print('best', best[0])
