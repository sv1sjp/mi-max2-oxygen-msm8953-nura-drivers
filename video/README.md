<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Hardware video decoding (Venus)

`install.sh --video` makes the MSM8953's video engine (Venus) decode video in hardware:
H.264 among others (the firmware also lists HEVC, VP8, VP9, MPEG-2/4, H.263, VC-1). Apps that
play through GStreamer use it automatically (`v4l2h264dec`, ...). Firefox can use it for
H.264 (see below).

It installs:

| What | Where |
|---|---|
| `venus-core.ko`, `venus-dec.ko` built with [`0004`](../kernel/patches/0004-media-venus-hfi_parser-tolerate-truncated-last-capability.patch) and [`0005`](../kernel/patches/0005-media-venus-msm8953-non-null-eos-address.patch) | `/lib/modules/<kernel>/updates/` |
| [`oxygen-venus.conf`](oxygen-venus.conf): `blacklist venus_enc` | `/etc/modprobe.d/oxygen-venus.conf` |

It needs Xiaomi's Venus firmware, which is signed for this phone and cannot be shipped:
`venus.mdt` and `venus.b00`–`venus.b04` in `/lib/firmware/`. They are in the `image/` folder
of the phone's modem (NON-HLOS) partition, for example from a partition backup:
`mcopy -i modem.img ::/image/venus.mdt ::/image/venus.b0? .` (mtools), then copy them to
`/lib/firmware/` on the phone. The ones used here are `VIDEO.VE.4.2-00045`.

## What was wrong

Without these patches the stock Nura kernel cannot use Venus at all:

1. **Probe fails (`0004`).** The firmware's SYS_INIT_DONE reply ends with a capability entry
   that is cut short (20 bytes needed, 12 left). The driver rejected the whole reply, so the
   probe failed with `-EIO` and no video device appeared.
2. **Every stream hangs at its end (`0005`).** To finish a stream, the driver queues an empty
   end-of-stream buffer. For this firmware generation (HFI v3) mainline sends it with address 0.
   Decompiling the firmware with Ghidra (via REA) showed why that fails here:
   `vDec_validateBuffer()` takes a session's content-protection class from its first buffer
   and requires every later buffer to match. The classifier (`FUN_00005310`, the
   "BufferChk" log) puts any address below the end of the firmware image (0x500000) into
   "CP_UNKNOWN, buffer lies in Firmware region". So address 0 does not match a normal
   session: the firmware logs "CP session is compromised", raises `HFI_ERR_SESSION_FATAL`
   and marks the session broken. GStreamer then waits forever for the decoder to stop.
   With the dummy address `0xdeadb000` that the driver already uses on other chips, the
   buffer counts as normal memory and the stream ends cleanly (3 of 3 runs, against 0 of 3).

Measured on the phone: a 720p H.264 file decodes at ~76 fps (with debug logging and an extra
copy in the test), 1080p decodes cleanly.

## Why the encoder stays off

The hardware **encoder** (for recording video) still fails: the firmware starts the
hardware for a frame ("VPP start HW" in its log) and the hardware never finishes, without
any error message. The firmware then stops answering, which also blocks the decoder until
the module is reloaded or the phone reboots. Ruled out so far: a missed doorbell interrupt
(a full barrier before reading `rx_req`, or always ringing it, changed nothing), and the
clock/voltage (capped at 228 or 310 MHz, no change). Also ruled out: the per-core wrapper power control
(`vcodec_control_v3()` writes the second core's register 0x4c for encoder sessions, and msm8953
has one core; using core 0's register or skipping the writes like Qualcomm's msm_vidc does made
no difference), the register presets (identical to Android's `qcom,reg-presets`) and the core
GDSC (hardware-controlled in both, `qcom,support-hw-trigger` / `HW_CTRL`). A fast input fails every time, slow
input only sometimes. So `oxygen-venus.conf` keeps the stock `venus_enc` from loading, and
Snapshot keeps recording with its software encoder (its "hardware encoding" option is off
by default).

## Firefox

Firefox 140 can decode **H.264** on Venus through V4L2 (its `v4l2test` probe reports
`V4L2_SUPPORTED TRUE` for `/dev/video6`). On Linux this path is off by default; turn it on with
`media.hardware-video-decoding.force-enabled = true` in `about:config` (or a `user.js` line) and
restart Firefox. YouTube sends VP9/AV1 by default, which Firefox only decodes on the CPU; an
add-on such as *enhanced-h264ify* makes it send H.264. Not verified yet: that Firefox then really
opens the Venus device while playing (check with `ls -l /proc/<RDD pid>/fd | grep video`).

## Undo

`sudo sh revert.sh --video`, then reboot.
