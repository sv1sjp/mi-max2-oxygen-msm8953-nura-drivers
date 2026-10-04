#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# TAS2560 speaker on QUATERNARY MI2S (port 0x1006, not hooked by oxygen's SmartAmp),
# like the upstream Lenovo ThinkSmart View. Boot: quat-ramboot.img (module_blacklist=q6afe).
# Own oxygen ADSP firmware. Sweeps the SD line: SD0..SD3 -> beeps-1..4.wav,
# sampling pads gpio88..95 during each playback (padsample.ko) to map SDn -> pad.
# Run as root. testboot.sh reboots afterwards.
amp() { i2ctransfer -f -y 0 w2@0x4c 0x00 0x00; i2ctransfer -f -y 0 w1@0x4c $1 r1; }
R=$(for r in /sys/class/remoteproc/remoteproc*; do grep -qx adsp $r/name && echo $r; done | head -1)
echo "== ADSP: $(cat $R/state) fw: $(cat $R/firmware)"

echo "== modules"
sh /home/user/topo-test.sh 2>&1 | grep -v "^$"
for i in $(seq 1 60); do amixer -c0 controls >/dev/null 2>&1 && break; sleep 1; done
echo "card after ${i}s: $(head -1 /proc/asound/cards)"
amixer -c0 controls >/dev/null 2>&1 || { echo "ABORT: no card"; dmesg | grep -iE "tas|asoc|q6|apr|sound|defer" | tail -30; exit 1; }

echo "== route (MultiMedia3 -> QUAT_MI2S_RX) + low gain"
sleep 10
mount -t debugfs none /sys/kernel/debug 2>/dev/null
route() {
  amixer -c0 cset name="QUAT_MI2S_RX Audio Mixer MultiMedia3" 0 >/dev/null
  amixer -c0 cset name="QUAT_MI2S_RX Audio Mixer MultiMedia3" 1 >/dev/null
  f=$(find /sys/kernel/debug/asoc -path "*/dapm/MM_DL3" 2>/dev/null | head -1)
  grep -q 'out  "MultiMedia3" "QUAT_MI2S_RX Audio Mixer"' "$f" && echo "route on (DAPM connected)" || echo "route NOT connected in DAPM"
}
route
amixer -c0 cset name="Speaker Amp Gain Volume" 3 >/dev/null && echo "gain 3 dB"

insmod /home/user/padsample.ko tag=idle 2>/dev/null
dmesg | grep "padsample idle" | tail -8
P=/sys/module/q6afe_topo/parameters/quat_sd_mask
for n in 0 1 2 3; do
  echo "== SD$n -> beeps-$((n + 1)).wav"
  echo $((1 << n)) > $P
  route
  ( aplay -D hw:0,2 /home/user/beeps-$((n + 1)).wav; aplay -D hw:0,2 /home/user/beeps-$((n + 1)).wav ) > /tmp/aplay.log 2>&1 &
  sleep 1
  insmod /home/user/padsample.ko tag=SD$n 2>/dev/null
  dmesg | grep "padsample SD$n" | tail -8
  echo "amp PWR(reg7)=$(amp 7) FLAGS38=$(amp 38) FLAGS39=$(amp 39)"
  wait
  echo "aplay: $(tail -1 /tmp/aplay.log)"
  dmesg | grep -E "Quaternary MI2S RX: SD mask|port 0x1006|-110" | tail -3
  if dmesg | grep -q -- "-110"; then echo "ABORT: DSP timeout"; break; fi
  sleep 3
done

echo "== dmesg"
dmesg | grep -iE "tas2560|asoc|q6afe|apr|-110|timeout|backend|Quaternary" | tail -25
echo "== ADSP: $(cat $R/state)"
sync
