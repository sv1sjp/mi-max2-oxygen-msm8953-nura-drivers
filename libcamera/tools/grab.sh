#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# usage: grab.sh <exposure-lines> <again-code 0..480> [vblank]   (run on the phone)
# Grabs one full-resolution raw IMX386 frame with a fixed exposure and analogue gain,
# bypassing libcamera, to /tmp/last.raw (view it with raw2png.py 4032 3016).
# Stop camera apps first; check /dev/v4l-subdev16 with `media-ctl -p`.
S=/dev/v4l-subdev16
v4l2-ctl -d $S -c vertical_blanking=${3:-54}
v4l2-ctl -d $S -c exposure=$1,analogue_gain=$2,test_pattern=0
v4l2-ctl -d /dev/video0 --set-fmt-video=width=4032,height=3016,pixelformat=pRAA
v4l2-ctl -d /dev/video0 --stream-mmap=4 --stream-count=5 --stream-skip=3 --stream-to=/tmp/g.raw >/dev/null 2>&1
tail -c 15200640 /tmp/g.raw > /tmp/last.raw
