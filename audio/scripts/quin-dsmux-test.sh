#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Quinary MI2S START with the LPASS mux registers set to the downstream state, pad sampling (quin-min-ramboot.img: vince fw, 8 mA pins), booted with audio-vince-ramboot.img: the ADSP boots
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

echo "== idle pads"
insmod /home/user/padsample.ko tag=idle 2>/dev/null; dmesg | grep "padsample idle" | tail -8
echo "== set LPASS mux registers to the downstream (Android) state"
insmod /home/user/lpassmux.ko addr=0x0c051004 clr=0xffffffff set=0x00010000 2>/dev/null
insmod /home/user/lpassmux.ko addr=0x0c051000 clr=0xffffffff set=0x00200000 2>/dev/null
insmod /home/user/lpassmux.ko addr=0x0c052000 clr=0xffffffff set=0x00000001 2>/dev/null
dmesg | grep "lpassmux after" | tail -4
echo "== Quinary START (MM3 -> QUIN_MI2S_RX), sampling pads during it"
route
aplay -D hw:0,2 /home/user/beeps-1.wav > /tmp/aplay.log 2>&1 &
sleep 0.4
insmod /home/user/padsample.ko tag=start+0.4s 2>/dev/null; dmesg | grep "padsample start+0.4s" | tail -8
echo "amp PWR(reg7)=$(amp 7) FLAGS38=$(amp 38) FLAGS39=$(amp 39)"
sleep 1.5
insmod /home/user/padsample.ko tag=start+2s 2>/dev/null; dmesg | grep "padsample start+2s" | tail -8
echo "== full pad scan (TLMM pads 4..134, reserved ones skipped) while the interface is stuck"
# skip TZ-reserved pads 0-3 and 135-138 (oxygen gpio-reserved-ranges) and non-existent 142+
for f in $(seq 4 8 124) 127; do insmod /home/user/padsample.ko first=$f samples=100000 tag=scan 2>/dev/null; done
dmesg | grep "padsample scan" | grep ACTIVE
echo "(scan done: $(dmesg | grep -c 'padsample scan') pads sampled)"
wait
echo "aplay: $(tail -1 /tmp/aplay.log)"

echo "== dmesg"
dmesg | grep -iE "tas|asoc|q6|apr|afe|lpass|remoteproc|-110|timeout|backend|MultiMedia" | tail -30
echo "== ADSP still alive?"
cat $R/state
amixer -c0 cset name="QUIN_MI2S_RX Audio Mixer MultiMedia3" 0 >/dev/null && echo "mixer ok (DSP responsive)"
sync
