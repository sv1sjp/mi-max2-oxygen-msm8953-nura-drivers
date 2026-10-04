#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Manual focus sweep on the phone: for each lens position given (DW9763 DAC codes,
# e.g. `sh sweep.sh $(seq 352 16 656)`), move the lens, grab raw frames from the
# IMX386 and keep a central crop in /tmp/af-<pos>.crop. Copy those to a PC and score
# them with af_sharpness.py. Stop camera apps first. Check the subdev numbers with
# `media-ctl -p` (sensor /dev/v4l-subdev16, lens /dev/v4l-subdev17 on our phone).
L=/dev/v4l-subdev17
exec 3<$L            # keep the VCM powered
rm -f /tmp/af-*.crop
for pos in $*; do
  v4l2-ctl -d $L -c focus_absolute=$pos
  sleep 0.3
  v4l2-ctl -d /dev/video0 --stream-mmap=4 --stream-count=3 --stream-skip=2 --stream-to=/tmp/s.raw >/dev/null 2>&1
  tail -c 15200640 /tmp/s.raw | dd bs=5040 skip=1000 count=1000 of=/tmp/af-$pos.crop 2>/dev/null
done
exec 3<&-
