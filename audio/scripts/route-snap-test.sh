#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Capture the DMA<->interface ROUTING block (0x0c0d0000) and the read-DMA channels
# (0x0c0d2000+) during WORKING Quaternary playback, then during the Quinary freeze.
# Quaternary is a real I2S interface that works via the DSP, so its routing registers
# reveal the exact DMA->interface encoding to replicate for Quinary. This is the one
# window every earlier snapshot missed (they stopped at 0x0c0d0000).
# Control-register space only (0x0c0d0000..0x0c0d3000) - no I2S FIFO/status regs, so it
# will not trigger the test-#24 read-hang. Boot: quat-ramboot.img. Run as root.
R=$(for r in /sys/class/remoteproc/remoteproc*; do grep -qx adsp $r/name && echo $r; done | head -1)
echo "== ADSP: $(cat $R/state) fw: $(cat $R/firmware)"
mount -t debugfs none /sys/kernel/debug 2>/dev/null
snap() { insmod /home/user/regsnap.ko addr=$2 size=$3 tag=$1 && cp /sys/kernel/debug/regsnap/$1.bin /home/user/rt-$1.bin; rmmod regsnap; chmod 644 /home/user/rt-$1.bin; echo "snap $1: $(ls -la /home/user/rt-$1.bin | awk '{print $5}') bytes"; }
dumproute() {   # print the routing block + each DMA channel's control regs from a snap
  python3 - "$1" <<'PY'
import struct,sys
b=open("/home/user/rt-%s.bin"%sys.argv[1],'rb').read(); base=0x0c0d0000
def r(a): return struct.unpack_from('<I',b,a-base)[0]
print("  routing:", " ".join("%08x"%r(0x0c0d0000+i*4) for i in range(8)))
print("  0x0c0d1000:", "%08x"%r(0x0c0d1000))
for ch in range(4):
    c=0x0c0d2000+ch*0x1000
    print("  dma%d CTL=%08x BASE=%08x BUFF=%08x CURR=%08x PER=%08x"%(ch,r(c),r(c+4),r(c+8),r(c+0xc),r(c+0x10)))
PY
}
sh /home/user/topo-test.sh 2>&1 | grep -v "^$"
for i in $(seq 1 60); do amixer -c0 controls >/dev/null 2>&1 && break; sleep 1; done
echo "card after ${i}s: $(head -1 /proc/asound/cards)"
amixer -c0 controls >/dev/null 2>&1 || { echo "ABORT: no card"; dmesg | grep -iE "tas|asoc|q6|apr" | tail -20; exit 1; }
sleep 10
route() { amixer -c0 cset name="$1 Audio Mixer MultiMedia3" 0 >/dev/null; amixer -c0 cset name="$1 Audio Mixer MultiMedia3" 1 >/dev/null
  f=$(find /sys/kernel/debug/asoc -path "*/dapm/MM_DL3" | head -1); grep -q "out  \"MultiMedia3\" \"$1 Audio Mixer\"" "$f" && echo "$1 route on" || echo "$1 route NOT connected"; }
unroute() { amixer -c0 cset name="$1 Audio Mixer MultiMedia3" 0 >/dev/null; }

echo "== IDLE routing (before any playback)"
snap idle 0x0c0d0000 0x3000; dumproute idle

echo "== Quaternary playback (WORKS) - capture its routing"
route QUAT_MI2S_RX
echo 1 > /sys/module/q6afe_topo/parameters/quat_sd_mask 2>/dev/null
aplay -D hw:0,2 -f S16_LE -c 2 -r 48000 -d 8 /dev/zero > /tmp/a1.log 2>&1 &
sleep 1.8
snap quat 0x0c0d0000 0x3000; dumproute quat
wait; echo "quat aplay: $(tail -1 /tmp/a1.log)"
unroute QUAT_MI2S_RX; sleep 2

echo "== Quinary START (freezes) - capture what is missing"
route QUIN_MI2S_RX
aplay -D hw:0,2 -f S16_LE -c 2 -r 48000 -d 6 /dev/zero > /tmp/a2.log 2>&1 &
sleep 1.8
snap quin 0x0c0d0000 0x3000; dumproute quin
wait; echo "quin aplay: $(tail -1 /tmp/a2.log)"
dmesg | grep -E "0x1016|0x1006|-110" | tail -3
sync; echo "done"
