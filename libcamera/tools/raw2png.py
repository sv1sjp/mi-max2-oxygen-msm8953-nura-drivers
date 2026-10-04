#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Quick RAW10 CSI2-packed RGGB -> half-res sRGB PNG (black 64, gray-world WB).

usage: raw2png.py <file.raw> <width> <height> [bayer order, default RGGB] [out.png]
For looking at frames from grab.sh or v4l2-ctl without libcamera, e.g. to check
a sensor mode, exposure or the Bayer order.
"""
import sys, numpy as np
from PIL import Image
path, w, h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
order = sys.argv[4] if len(sys.argv) > 4 else 'RGGB'
stride = w * 10 // 8
b = np.fromfile(path, np.uint8).reshape(h, stride).astype(np.uint16)
b = b.reshape(h, w // 4, 5)
px = np.empty((h, w // 4, 4), np.uint16)
for i in range(4):
    px[:, :, i] = (b[:, :, i] << 2) | ((b[:, :, 4] >> (2 * i)) & 3)
px = px.reshape(h, w).astype(np.float32)
print('raw min/mean/max', px.min(), px.mean().round(1), px.max(), 'clipped %', (px >= 1023).mean() * 100)
px = np.clip(px - 64, 0, None) / (1023 - 64)
q = {order[0]: px[0::2, 0::2], order[1]: px[0::2, 1::2], order[3]: px[1::2, 1::2]}
g = (px[0::2, 1::2] + px[1::2, 0::2]) / 2 if order in ('RGGB', 'BGGR') else (px[0::2, 0::2] + px[1::2, 1::2]) / 2
r, bl = q['R'], q['B']
rgb = np.stack([r * g.mean() / r.mean(), g, bl * g.mean() / bl.mean()], -1)
rgb = rgb / np.percentile(rgb, 99.5)
rgb = np.clip(rgb, 0, 1) ** (1 / 2.2)
im = Image.fromarray((rgb * 255).astype(np.uint8))
im.rotate(-270 % 360, expand=True).save(sys.argv[5] if len(sys.argv) > 5 else path.rsplit('.', 1)[0] + '.png')
