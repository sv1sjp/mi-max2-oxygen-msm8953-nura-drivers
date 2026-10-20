#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Quinary MI2S START with live ADSP DIAG logging (adspdiag.ko), to see where the ADSP blocks.
# Boot audio-ramboot.img (oxygen fw, Quinary RX link + tas2560). Run as root; testboot.sh
# reboots to the normal OS afterwards. Output: /home/user/adspdiag/quin-cap.bin + quin-events.log
D=/home/user/adspdiag; EV=$D/quin-events.log
ev() { echo "$(cat /proc/uptime | cut -d' ' -f1) $*" | tee -a $EV; }
: > $EV
R=$(for r in /sys/class/remoteproc/remoteproc*; do grep -qx adsp "$r/name" && echo "$r"; done | head -1)
ev "adsp $(cat "$R/state")"
ev "hexagonrpcd at start: $(systemctl is-active hexagonrpcd-adsp-rootpd) $(journalctl -b -u hexagonrpcd-adsp-rootpd --no-pager -o short-monotonic | tail -20 | tr "\n" "|")"
mount -t debugfs none /sys/kernel/debug 2>/dev/null
insmod $D/adspdiag.ko
for c in DIAG DIAG_CNTL; do d=${R##*/}:smd-edge.$c.-1.-1
  echo adspdiag > /sys/bus/rpmsg/devices/"$d"/driver_override; echo "$d" > /sys/bus/rpmsg/drivers/adspdiag/bind; sleep 1; done
sleep 3; python3 $D/adspdiag-masks.py; sleep 1
python3 $D/adspdiag-raw.py $D/quin-cap.bin 100 &
sleep 2; ev "diag: $(tr '\n' ' ' < /sys/kernel/debug/adspdiag/stats)"

if [ "${FASTRPC:-0}" = 1 ]; then
  # Serve the ADSP's reverse FastRPC calls (SmartAmp's fopen of tas2560_*.bin) with the
  # stock hexagonrpcd (rootpd). Needs the FastRPC DT node (audio-ramboot.img) + fastrpc.ko.
  insmod /home/user/fastrpc.ko
  for i in $(seq 1 20); do [ -e /dev/fastrpc-adsp ] && break; sleep 1; done
  ev "fastrpc: $(ls /dev/fastrpc* 2>&1 | tr '\n' ' ')"
  systemctl start hexagonrpcd-adsp-rootpd; sleep 5
  ev "hexagonrpcd: $(systemctl is-active hexagonrpcd-adsp-rootpd)"
fi
ev "insmod tas2560"
insmod /home/user/tas2560.ko
for i in $(seq 1 60); do amixer -c0 controls >/dev/null 2>&1 && break; sleep 1; done
ev "card after ${i}s: $(head -1 /proc/asound/cards)"
sleep 10
amixer -c0 cset name="QUIN_MI2S_RX Audio Mixer MultiMedia3" 0 >/dev/null
amixer -c0 cset name="QUIN_MI2S_RX Audio Mixer MultiMedia3" 1 >/dev/null
amixer -c0 cset name="Speaker Amp Gain Volume" 3 >/dev/null
MMDL3=$(find /sys/kernel/debug/asoc -path "*/dapm/MM_DL3" 2>/dev/null | head -1)
grep -q 'out  "MultiMedia3" "QUIN_MI2S_RX Audio Mixer"' "$MMDL3" && ev "route on" || ev "route NOT connected"
sleep 3
ev "aplay start (Quinary START)"
aplay -D hw:0,2 /home/user/beeps-1.wav > /tmp/aplay.log 2>&1 &
sleep 20
ev "aplay: $(tail -1 /tmp/aplay.log)"
ev "dmesg: $(dmesg | grep -iE 'afe|q6|-110' | tail -6 | tr '\n' '|')"
ev "diag: $(tr '\n' ' ' < /sys/kernel/debug/adspdiag/stats)"
ev "adsp $(cat "$R/state")"
ev "hexagonrpcd at start: $(systemctl is-active hexagonrpcd-adsp-rootpd) $(journalctl -b -u hexagonrpcd-adsp-rootpd --no-pager -o short-monotonic | tail -20 | tr "\n" "|")"
[ "${FASTRPC:-0}" = 1 ] && ev "hexagonrpcd log: $(journalctl -b -u hexagonrpcd-adsp-rootpd --no-pager -o short-monotonic | tail -40 | tr '\n' '|')"
sleep 10
kill %1 2>/dev/null; sync
ls -la $D/quin-cap.bin
