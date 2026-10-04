# How to capture the working DSP command sequence from Android (for later)

> **READY-TO-RUN KIT (2026-09-30):** `android-capture-kit/` has a foolproof, self-verifying
> script (`capture-speaker-dsp.sh`) + `CAPTURE-README.txt` for a non-technical operator to run
> on ANY rooted msm8953 Android phone with a working loudspeaker (ideal: oxygen on Pixel
> Experience A13). It auto-detects tracefs/arch/`apr_send_pkt`, handles kptr_restrict + SELinux,
> captures the speaker-play APR trace + mixer/ACDB/fw context, and prints PASS/FAIL. Output:
> one file `speaker-dsp-capture.txt` to bring back. The manual method below is the same thing by hand.

Goal: record the exact APR packets the **working** Android audio path sends the ADSP when the
loudspeaker plays, so we can diff it against mainline q6afe and add what's missing. This is the
method that cracked the Fairphone 3/5 speaker (pmaports issue #3793). Source reading alone was
not enough there; this live capture was decisive.

Best source of the capture, in order:
1. This Mi Max 2 (oxygen) booted back to **Android/MIUI or LineageOS** — same DSP firmware and
   hardware, so the capture is directly applicable. (Risky now: failing eMMC, currently on pmOS.)
2. Any other **msm8953 Android phone with a working loudspeaker** (see device list in
   AUDIO-HANDOFF / QUINARY notes). Less exact but still very useful.

## Steps (rooted Android, adb)
```
adb root && adb shell        # or: su   in a terminal app
cd /sys/kernel/tracing        # or /sys/kernel/debug/tracing on older Android
echo 0 > tracing_on
echo 8000 > buffer_size_kb
echo nop > current_tracer
echo > kprobe_events
# apr_send_pkt(adev, pkt): pkt is in x1; grab opcode(+16), len(+2), first payload words
echo 'p:kp apr_send_pkt+0 op=+16(%x1):x32 len=+2(%x1):u16 p1=+20(%x1):x64 p2=+28(%x1):x64 p3=+36(%x1):x64 p4=+44(%x1):x64 p5=+52(%x1):x64 p6=+60(%x1):x64 p7=+68(%x1):x64 p8=+76(%x1):x64' >> kprobe_events
echo 1 > events/kprobes/kp/enable
echo 1 > events/printk/console/enable
echo 1 > tracing_on
# --- now, from silence, start LOUDSPEAKER playback (a song / ringtone on speaker) ---
# let it play a few seconds, then:
cat trace > /sdcard/apr-speaker.txt
echo 0 > tracing_on
```
Pull it: `adb pull /sdcard/apr-speaker.txt` and give it to me.

## If apr_send_pkt has a different signature / symbol
Downstream sometimes exports `apr_send_pkt(void *handle, uint32_t *buf)` (buf still in x1), or
the symbol is in a module with a mangled name. If `apr_send_pkt` is not found:
```
echo 0 > /proc/sys/kernel/kptr_restrict
grep ' apr_send_pkt' /proc/kallsyms      # note the address, e.g. ffffffd2669041a4
# then use that address in the kprobe: 'p:kp 0xffffffd2669041a4 op=+16(%x1):x32 ...'
```

## What I do with it
Decode each opcode (AFE_PORT_CMD_DEVICE_START 0x100e5, AFE_PORT_CMD_SET_PARAM_V2 0x100ef,
AFE_SVC_CMD_SET_PARAM 0x100f3, etc.) around the Quinary port (0x1016) enable, and compare with
the packets mainline q6afe sends. The delta (a SmartAmp SET_PARAM, an ACDB calibration blob, or
an ordering/clock step) is what to add to the mainline q6afe patch so the intended path works
instead of hanging. See AUDIO-HANDOFF.md "Session 3" for the current q6afe understanding.
