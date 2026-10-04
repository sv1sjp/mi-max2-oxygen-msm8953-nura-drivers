#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Snapshot LPAIF (0x0c0c0000 +64K) and the Quinary clock block (0x0c032000 +4K)
# during Quaternary playback (works) and during the Quinary freeze. Only snapshots
# while an interface is running (reading LPAIF with its clocks off could hang the bus).
R=$(for r in /sys/class/remoteproc/remoteproc*; do grep -qx adsp $r/name && echo $r; done | head -1)
echo "== ADSP: $(cat $R/state) fw: $(cat $R/firmware)"
mount -t debugfs none /sys/kernel/debug 2>/dev/null
snap() { insmod /home/user/regsnap.ko addr=$2 size=$3 tag=$1 && cp /sys/kernel/debug/regsnap/$1.bin /home/user/snap-$1.bin; rmmod regsnap; chmod 644 /home/user/snap-$1.bin; echo "snap $1: $(ls -la /home/user/snap-$1.bin | awk '{print $5}') bytes"; }
insmod /home/user/tas2560.ko
for i in $(seq 1 60); do amixer -c0 controls >/dev/null 2>&1 && break; sleep 1; done
echo "card after ${i}s"; sleep 10
route() { amixer -c0 cset name="$1 Audio Mixer MultiMedia3" 0 >/dev/null; amixer -c0 cset name="$1 Audio Mixer MultiMedia3" 1 >/dev/null
  f=$(find /sys/kernel/debug/asoc -path "*/dapm/MM_DL3" | head -1); grep -q "out  \"MultiMedia3\" \"$1 Audio Mixer\"" "$f" && echo "$1 route on" || echo "$1 route NOT connected"; }
unroute() { amixer -c0 cset name="$1 Audio Mixer MultiMedia3" 0 >/dev/null; }

echo "== Quaternary playback (works)"
route QUAT_MI2S_RX
aplay -D hw:0,2 -f S16_LE -c 2 -r 48000 -d 6 /dev/zero > /tmp/a1.log 2>&1 &
sleep 1.5
snap quat-lpaif 0x0c0c0000 0x10000
snap quat-quiclk 0x0c032000 0x1000
wait; echo "quat aplay: $(tail -1 /tmp/a1.log)"
unroute QUAT_MI2S_RX; sleep 2

echo "== Quinary START (freezes)"
route QUIN_MI2S_RX
aplay -D hw:0,2 -f S16_LE -c 2 -r 48000 -d 6 /dev/zero > /tmp/a2.log 2>&1 &
sleep 1.5
snap quin-lpaif 0x0c0c0000 0x10000
snap quin-quiclk 0x0c032000 0x1000
wait; echo "quin aplay: $(tail -1 /tmp/a2.log)"
dmesg | grep -E "0x1016|0x1006|-110" | tail -4
sync
