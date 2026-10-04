#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Read-only: what does the ADSP write into the LPAIF I2S slots during its Quinary START?
# Boot: quin-min-ramboot.img (vince fw). Uses quinws.ko dry=1 (single-register reads only).
R=$(for r in /sys/class/remoteproc/remoteproc*; do grep -qx adsp $r/name && echo $r; done | head -1)
echo "== ADSP: $(cat $R/state) fw: $(cat $R/firmware)"
mount -t debugfs none /sys/kernel/debug 2>/dev/null
insmod /home/user/tas2560.ko
for i in $(seq 1 60); do amixer -c0 controls >/dev/null 2>&1 && break; sleep 1; done
echo "card after ${i}s"; sleep 10
M="QUIN_MI2S_RX Audio Mixer MultiMedia3"
amixer -c0 cset name="$M" 0 >/dev/null; amixer -c0 cset name="$M" 1 >/dev/null
f=$(find /sys/kernel/debug/asoc -path "*/dapm/MM_DL3" | head -1)
grep -q 'out  "MultiMedia3" "QUIN_MI2S_RX Audio Mixer"' "$f" && echo "route on" || echo "route NOT connected"
snap() { insmod /home/user/quinws.ko dry=1 2>/dev/null; echo "--- $1"; dmesg | grep "quinws: before" | tail -2 | sed 's/^\[[^]]*\] //'; }
snap "idle (card up, before START)"
aplay -D hw:0,2 -f S16_LE -c 2 -r 48000 -d 8 /dev/zero > /tmp/a.log 2>&1 &
sleep 0.3; snap "START +0.3 s"
sleep 0.7; snap "START +1 s"
sleep 1.5; snap "START +2.5 s"
wait; echo "aplay: $(tail -1 /tmp/a.log)"
dmesg | grep -E "0x1016|-110" | head -3
sync
