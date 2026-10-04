#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Does PRIMARY MI2S drive the pri_mi2s pads (88/91/92/93/94/95) that the TAS2560 sits on?
# Boot: audio-prishare-ramboot.img (own oxygen ADSP fw, amp on the Primary link, sd-lines <2>).
# Samples the pads during Primary playback. Primary never hangs the ADSP. Run as root.
amp() { i2ctransfer -f -y 0 w2@0x4c 0x00 0x00; i2ctransfer -f -y 0 w1@0x4c $1 r1; }
R=$(for r in /sys/class/remoteproc/remoteproc*; do grep -qx adsp $r/name && echo $r; done | head -1)
echo "== ADSP: $(cat $R/state) fw: $(cat $R/firmware)"

insmod /home/user/tas2560.ko
for i in $(seq 1 60); do amixer -c0 controls >/dev/null 2>&1 && break; sleep 1; done
echo "card after ${i}s: $(head -1 /proc/asound/cards)"
amixer -c0 controls >/dev/null 2>&1 || { echo "ABORT: no card"; dmesg | grep -iE "tas|asoc|q6|sound|defer" | tail -30; exit 1; }

sleep 10
mount -t debugfs none /sys/kernel/debug 2>/dev/null
route() {
  amixer -c0 cset name="PRI_MI2S_RX Audio Mixer MultiMedia1" 0 >/dev/null
  amixer -c0 cset name="PRI_MI2S_RX Audio Mixer MultiMedia1" 1 >/dev/null
  f=$(find /sys/kernel/debug/asoc -path "*/dapm/MM_DL1" 2>/dev/null | head -1)
  grep -q 'out  "MultiMedia1" "PRI_MI2S_RX Audio Mixer"' "$f" && echo "route on (DAPM connected)" || echo "route NOT connected in DAPM"
}
amixer -c0 cset name="Speaker Amp Gain Volume" 3 >/dev/null && echo "gain 3 dB"
insmod /home/user/lpassmux.ko 2>/dev/null; dmesg | grep "lpassmux before" | tail -4
insmod /home/user/padsample.ko tag=idle 2>/dev/null; dmesg | grep "padsample idle" | grep -c ACTIVE | sed 's/^/idle ACTIVE pads: /'

echo "== Primary playback (hw:0,0), sampling pads"
route
( aplay -D hw:0,0 /home/user/beeps-1.wav; aplay -D hw:0,0 /home/user/beeps-1.wav ) > /tmp/aplay.log 2>&1 &
sleep 0.5
insmod /home/user/padsample.ko tag=pri 2>/dev/null; dmesg | grep "padsample pri" | tail -8
echo "amp PWR(reg7)=$(amp 7) FLAGS38=$(amp 38) FLAGS39=$(amp 39)"
wait
echo "aplay: $(tail -1 /tmp/aplay.log)"
insmod /home/user/lpassmux.ko 2>/dev/null; dmesg | grep "lpassmux before" | tail -4

echo "== dmesg"
dmesg | grep -iE "tas2560|asoc|q6afe|-110|timeout|backend" | tail -15
echo "== ADSP: $(cat $R/state)"
sync
