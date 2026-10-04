#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# LPASS clock-controller snapshots: idle, then during playback on $PORT ("quat" or "quin").
# Snapshots are saved to /home/user/lpasscc-<boot>-<state>.bin. Run as root.
PORT=$1
R=$(for r in /sys/class/remoteproc/remoteproc*; do grep -qx adsp $r/name && echo $r; done | head -1)
echo "== ADSP: $(cat $R/state) fw: $(cat $R/firmware)"
mount -t debugfs none /sys/kernel/debug 2>/dev/null
snap() { insmod /home/user/regsnap.ko tag=$1 && cp /sys/kernel/debug/regsnap/$1.bin /home/user/lpasscc-$1.bin; rmmod regsnap; echo "snap $1: $(ls -la /home/user/lpasscc-$1.bin | awk '{print $5}') bytes"; }
insmod /home/user/tas2560.ko
for i in $(seq 1 60); do amixer -c0 controls >/dev/null 2>&1 && break; sleep 1; done
echo "card after ${i}s"
sleep 10
if [ "$PORT" = quat ]; then MIX="QUAT_MI2S_RX Audio Mixer MultiMedia3"; T=QUAT_MI2S_RX; else MIX="QUIN_MI2S_RX Audio Mixer MultiMedia3"; T=QUIN_MI2S_RX; fi
amixer -c0 cset name="$MIX" 0 >/dev/null; amixer -c0 cset name="$MIX" 1 >/dev/null
f=$(find /sys/kernel/debug/asoc -path "*/dapm/MM_DL3" | head -1)
grep -q "out  \"MultiMedia3\" \"$T Audio Mixer\"" "$f" && echo "route on (DAPM connected)" || echo "route NOT connected"
amixer -c0 cset name="Speaker Amp Gain Volume" 3 >/dev/null
snap $PORT-idle
aplay -D hw:0,2 -f S16_LE -c 2 -r 48000 -d 12 /dev/zero > /tmp/aplay.log 2>&1 &
sleep 1.5
snap $PORT-play
insmod /home/user/padsample.ko tag=$PORT 2>/dev/null; dmesg | grep "padsample $PORT" | grep ACTIVE
wait
echo "aplay: $(tail -1 /tmp/aplay.log)"
dmesg | grep -E "0x1016|0x1006|-110" | tail -4
sync
