#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Capture the LPAIF routing region (0x0c0cf000+0x3000) during working Quaternary playback,
# to compare vs the internal-codec routing and decode the DMA<->interface selection.
# Boot: quat-ramboot.img. Read-only snapshot. Run as root.
R=$(for r in /sys/class/remoteproc/remoteproc*; do grep -qx adsp $r/name && echo $r; done | head -1)
echo "== ADSP: $(cat $R/state)"
mount -t debugfs none /sys/kernel/debug 2>/dev/null
sh /home/user/topo-test.sh 2>&1 | grep -v "^$"
for i in $(seq 1 60); do amixer -c0 controls >/dev/null 2>&1 && break; sleep 1; done
echo "card after ${i}s"; sleep 8
amixer -c0 cset name="QUAT_MI2S_RX Audio Mixer MultiMedia3" 1 >/dev/null
echo 1 > /sys/module/q6afe_topo/parameters/quat_sd_mask 2>/dev/null
snap() { insmod /home/user/regsnap.ko addr=0x0c0cf000 size=0x3000 tag=$1 && cp /sys/kernel/debug/regsnap/$1.bin /home/user/rt-$1.bin; rmmod regsnap; chmod 644 /home/user/rt-$1.bin; }
snap quat-idle
aplay -D hw:0,2 -f S16_LE -c 2 -r 48000 -d 8 /dev/zero > /tmp/a.log 2>&1 &
sleep 1.5
snap quat-play
wait
echo "quat aplay: $(tail -1 /tmp/a.log)"
echo "done"; sync
