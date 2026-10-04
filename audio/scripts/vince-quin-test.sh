#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Quinary MI2S playback test with the vince (Redmi 5 Plus) ADSP firmware, which has no SmartAmp.
# Run as root in the audio-ramboot.img test boot. Firmware is staged in /run (RAM) only.
R=/sys/class/remoteproc/remoteproc2
FW=/run/msm-firmware-loader/target/vince
amp() { i2ctransfer -f -y 0 w2@0x4c 0x00 0x00; i2ctransfer -f -y 0 w1@0x4c $1 r1; }

echo "== swap ADSP firmware to vince"
mkdir -p $FW && cp /home/user/vince-adsp/adsp.* $FW/
WCD=/sys/bus/platform/drivers/msm8916-wcd-digital-codec
timeout 90 sh -c "echo stop > $R/state"
for i in $(seq 1 60); do grep -q offline $R/state && break; sleep 1; done
echo "after stop: $(cat $R/state)"
if ! grep -q offline $R/state; then echo "ABORT: ADSP did not stop"; exit 1; fi
sleep 3
echo vince/adsp.mdt > $R/firmware
timeout 60 sh -c "echo start > $R/state"
for i in $(seq 1 30); do grep -q running $R/state && break; sleep 1; done
sleep 8; echo "adsp: $(cat $R/state) fw: $(cat $R/firmware)"
dmesg | grep -i "Booting fw\|Boot failed" | tail -2
if ! grep -q running $R/state; then echo "ABORT: vince ADSP not running"; exit 1; fi
# the WCD digital codec still holds the old ADSP's (now unregistered) clock: rebind it
echo c0f0000.codec > $WCD/unbind && echo "wcd digital codec unbound"; sleep 1
echo c0f0000.codec > $WCD/bind && echo "wcd digital codec rebound"; sleep 3

echo "== amp driver + card"
insmod /home/user/tas2560.ko
for i in $(seq 1 60); do amixer -c0 controls >/dev/null 2>&1 && break; sleep 1; done
echo "card after ${i}s:"; cat /proc/asound/cards
if ! amixer -c0 controls >/dev/null 2>&1; then
  echo "== NO CARD: diagnostics"
  lsmod | grep -E "tas2560|snd_soc|q6|apr" | awk '{print $1}' | tr "\n" " "; echo
  mount -t debugfs none /sys/kernel/debug 2>/dev/null; cat /sys/kernel/debug/devices_deferred
  ls /sys/bus/apr/devices/ /sys/bus/i2c/devices/ 2>/dev/null | tr "\n" " "; echo
  dmesg | grep -iE "tas|snd|asoc|q6|apr|sound|lpass|wcd|probe|defer" | tail -50
fi
amixer -c0 controls | grep -i "amp\|QUIN_MI2S_RX Audio Mixer MultiMedia3\|ASI"

echo "== route + low gain"
amixer -c0 cset name="QUIN_MI2S_RX Audio Mixer MultiMedia3" 1
amixer -c0 controls | grep -o "name='[^']*Amp Gain Volume'" | cut -d"'" -f2 | while read n; do amixer -c0 cset name="$n" 3; done

echo "== play beeps-1 on Quinary (hw:0,2) x6"
( for i in 1 2 3 4 5 6; do aplay -D hw:0,2 /home/user/beeps-1.wav || break; done ) > /tmp/aplay.log 2>&1 &
sleep 4
echo "amp PWR(reg7)=$(amp 7) FLAGS38=$(amp 38) FLAGS39=$(amp 39)"
sleep 20
echo "amp PWR(reg7)=$(amp 7) FLAGS38=$(amp 38) FLAGS39=$(amp 39)"
wait
cat /tmp/aplay.log | tail -5
echo "== dmesg"
dmesg | grep -iE "tas|snd|asoc|q6|apr|afe|sound|lpass|remoteproc|-110|timeout" | tail -40
echo "== ADSP still alive?"
cat $R/state
amixer -c0 cset name="QUIN_MI2S_RX Audio Mixer MultiMedia3" 0 >/dev/null && echo "mixer ok"
# never stop the vince ADSP (that hung the phone once); just sync and let testboot reboot
sync
