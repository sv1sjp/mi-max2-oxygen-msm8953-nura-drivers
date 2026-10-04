#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Quinary MI2S playback test, booted with audio-vince-ramboot.img: the ADSP boots
# directly on the vince (Redmi 5 Plus) firmware (no SmartAmp), no runtime swap.
# Run as root. Never stops the ADSP; testboot.sh reboots afterwards.
R=$(for r in /sys/class/remoteproc/remoteproc*; do grep -qx adsp $r/name && echo $r; done | head -1)
amp() { i2ctransfer -f -y 0 w2@0x4c 0x00 0x00; i2ctransfer -f -y 0 w1@0x4c $1 r1; }

echo "== ADSP"
echo "adsp: $(cat $R/state) fw: $(cat $R/firmware)"
dmesg | grep -iE "Booting fw|Boot failed|adsp is now up" | tail -3
if ! grep -q running $R/state; then echo "ABORT: ADSP not running"; dmesg | grep -iE "remoteproc|mdt|pas|firmware" | tail -20; exit 1; fi

echo "== amp driver + card"
insmod /home/user/tas2560.ko
for i in $(seq 1 60); do amixer -c0 controls >/dev/null 2>&1 && break; sleep 1; done
echo "card after ${i}s:"; cat /proc/asound/cards | head -1
if ! amixer -c0 controls >/dev/null 2>&1; then
  echo "== NO CARD"; dmesg | grep -iE "tas|snd|asoc|q6|apr|sound|lpass|wcd|defer|mclk" | tail -40; exit 1
fi

echo "== route + low gain"
sleep 10   # let the card settle (it can be re-created right after it first appears)
mount -t debugfs none /sys/kernel/debug 2>/dev/null
MMDL3=$(find /sys/kernel/debug/asoc -path "*/dapm/MM_DL3" 2>/dev/null | head -1)
route() {
  # q6routing put() skips the DAPM connect if its session already points at this port: toggle
  amixer -c0 cset name="QUIN_MI2S_RX Audio Mixer MultiMedia3" 0 >/dev/null
  amixer -c0 cset name="QUIN_MI2S_RX Audio Mixer MultiMedia3" 1 >/dev/null
  MMDL3=$(find /sys/kernel/debug/asoc -path "*/dapm/MM_DL3" 2>/dev/null | head -1)
  grep -q 'out  "MultiMedia3" "QUIN_MI2S_RX Audio Mixer"' "$MMDL3" && echo "route on (DAPM connected)" || echo "route NOT connected in DAPM"
}
echo "MM3 routes before:"; amixer -c0 contents | awk -F"name=" '/^numid/{n=($2 ~ /MultiMedia3.$/)?$2:""} /: values=on/ && n!=""{print "  " n}'
route
amixer -c0 cset name="Speaker Amp Gain Volume" 3 >/dev/null && echo "gain 3 dB"

echo "== DAPM state"
D=$(ls -d /sys/kernel/debug/asoc/*/ 2>/dev/null | head -1)
for w in "MultiMedia3 Playback" "MM_DL3" "QUIN_MI2S_RX Audio Mixer" "QUIN_MI2S_RX" "Quinary MI2S Playback" "Speaker ASI1 Playback" "Speaker ASI1" "Speaker ClassD" "Speaker OUT"; do
  f=$(find "$D" -path "*/dapm/$w" 2>/dev/null | head -1)
  [ -n "$f" ] && { echo "--- $f"; head -12 "$f"; } || echo "--- (no widget '$w')"
done

echo "== QDSP6SS 0x0c20002c bit 3 (FP3/SDM632 PAS-vs-PIL difference): clear before START"
insmod /home/user/qdsp6ss_bit.ko clear=1 2>/dev/null
dmesg | grep "qdsp6ss_bit: reg\|qdsp6ss_bit: cleared" | tail -2

echo "== play beeps-1 on Quinary (hw:0,2), retries"
for t in 1 2 3; do
  route
  aplay -D hw:0,2 /home/user/beeps-1.wav > /tmp/aplay.log 2>&1 && { echo "aplay try $t: OK"; break; }
  echo "aplay try $t: $(tail -1 /tmp/aplay.log)"; sleep 15
done
( for i in 1 2 3 4 5; do aplay -D hw:0,2 /home/user/beeps-1.wav || break; done ) >> /tmp/aplay.log 2>&1 &
sleep 4
echo "amp PWR(reg7)=$(amp 7) FLAGS38=$(amp 38) FLAGS39=$(amp 39)"
sleep 5
echo "amp PWR(reg7)=$(amp 7) FLAGS38=$(amp 38) FLAGS39=$(amp 39)"
wait
echo "aplay:"; tail -5 /tmp/aplay.log

echo "== dmesg"
dmesg | grep -iE "tas|asoc|q6|apr|afe|lpass|remoteproc|-110|timeout|backend|MultiMedia" | tail -30
echo "== ADSP still alive?"
cat $R/state
amixer -c0 cset name="QUIN_MI2S_RX Audio Mixer MultiMedia3" 0 >/dev/null && echo "mixer ok (DSP responsive)"
sync
