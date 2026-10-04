#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Can working Quaternary MI2S be routed to the amp's pri_mi2s pads (gpio88/91/92/93)?
# Boot: quat-ramboot.img (oxygen fw, Quaternary link, module_blacklist=q6afe). Run as root.
# Plays Quaternary via the DSP (works), scans ALL pads to find where Quaternary comes out,
# then tries the LPASS mux registers that could steer it to the amp pads.
amp() { i2ctransfer -f -y 0 w2@0x4c 0x00 0x00; i2ctransfer -f -y 0 w1@0x4c $1 r1; }
R=$(for r in /sys/class/remoteproc/remoteproc*; do grep -qx adsp $r/name && echo $r; done | head -1)
echo "== ADSP: $(cat $R/state) fw: $(cat $R/firmware)"
mount -t debugfs none /sys/kernel/debug 2>/dev/null
sh /home/user/topo-test.sh 2>&1 | grep -v "^$"
for i in $(seq 1 60); do amixer -c0 controls >/dev/null 2>&1 && break; sleep 1; done
echo "card after ${i}s: $(head -1 /proc/asound/cards)"
amixer -c0 controls >/dev/null 2>&1 || { echo "ABORT: no card"; exit 1; }
sleep 8

echo "== route Quaternary + SD0 + low gain"
amixer -c0 cset name="QUAT_MI2S_RX Audio Mixer MultiMedia3" 1 >/dev/null
echo 1 > /sys/module/q6afe_topo/parameters/quat_sd_mask 2>/dev/null
amixer -c0 cset name="Speaker Amp Gain Volume" 3 >/dev/null

scan() {  # scan pads 4..134, print only ACTIVE ones with a tag
  for f in $(seq 4 8 124) 127; do insmod /home/user/padsample.ko first=$f samples=80000 tag=$1 2>/dev/null; done
  dmesg | grep "padsample $1" | grep ACTIVE | sed 's/^\[[^]]*\] //'
}
mux() { insmod /home/user/lpassmux.ko 2>/dev/null; dmesg | grep "lpassmux before" | tail -4 | sed 's/^\[[^]]*\] //'; }

echo "== start Quaternary playback"
( for i in $(seq 1 20); do aplay -D hw:0,2 -f S16_LE -c 2 -r 48000 -d 6 /dev/zero || break; done ) > /tmp/a.log 2>&1 &
sleep 2
echo "amp PWR=$(amp 7)  aplay: $(tail -1 /tmp/a.log)"
echo "-- mux registers now:"; mux
echo "-- pads active during Quaternary (baseline, before any mux poke):"
scan qbase

echo "== TRY 1: mic_iomux |= QUA_WS_SLAVE_SEL(bit17)+SCLK(bit1) [mainline apq8016 quaternary mux]"
insmod /home/user/lpassmux.ko addr=0x0c051000 clr=0 set=0x00020002 2>/dev/null; dmesg | grep "lpassmux after" | tail -1 | sed 's/^\[[^]]*\] //'
scan qtry1

echo "== TRY 2: pri mode muxsel 0x0c055000 = 1"
insmod /home/user/lpassmux.ko addr=0x0c055000 clr=0 set=0x00000001 2>/dev/null; dmesg | grep "lpassmux after" | tail -1 | sed 's/^\[[^]]*\] //'
scan qtry2

echo "== amp status + speaker check window (listen ~now for tone if any pad reached the amp)"
amixer -c0 cset name="Speaker Amp Gain Volume" 5 >/dev/null
( for i in 1 2 3; do aplay -D hw:0,2 /home/user/beeps-1.wav; done ) >> /tmp/a.log 2>&1
echo "amp PWR=$(amp 7) FLAGS38=$(amp 38) FLAGS39=$(amp 39)"
echo "== dmesg tail"; dmesg | grep -iE "q6afe|-110|quat|QUAT" | tail -6
kill %1 2>/dev/null; wait 2>/dev/null
sync
