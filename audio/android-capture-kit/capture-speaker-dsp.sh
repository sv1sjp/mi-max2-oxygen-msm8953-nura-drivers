#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# ============================================================================
#  msm8953 loudspeaker DSP capture  --  run on a ROOTED Android phone
# ============================================================================
# Purpose: record the exact commands Android's audio driver sends the DSP when
# the LOUDSPEAKER plays. This reveals what the postmarketOS mainline driver is
# missing (its Quinary speaker port hangs). Best device: Xiaomi Mi Max 2
# (oxygen) on Android; otherwise ANY msm8953 phone with a working loudspeaker.
#
# You do NOT need to understand any of this. Just run it and follow the prompts.
# It writes ONE result file and prints PASS or FAIL at the end. Send the file back.
#
# HOW TO RUN (pick one):
#   A) adb:     adb push capture-speaker-dsp.sh /data/local/tmp/
#               adb shell su -c 'sh /data/local/tmp/capture-speaker-dsp.sh'
#   B) Termux (with root): su   then   sh /sdcard/capture-speaker-dsp.sh
# ============================================================================
set -u
OUT=/sdcard/speaker-dsp-capture.txt
TMP=/data/local/tmp/_spk_cap
mkdir -p "$TMP" 2>/dev/null
: > "$OUT" 2>/dev/null || OUT=/data/local/tmp/speaker-dsp-capture.txt
log() { echo "$@"; echo "$@" >> "$OUT"; }
raw() { echo "$@" >> "$OUT"; }

log "=== msm8953 speaker DSP capture  $(date 2>/dev/null) ==="

# ---- 0. root check --------------------------------------------------------
if [ "$(id -u 2>/dev/null)" != "0" ]; then
  echo "!! NOT ROOT. Re-run as root:  su -c 'sh $0'   (or 'su' first, then run)."
  exit 1
fi

# ---- 1. device context (helps correlate the trace) ------------------------
log "--- device ---"
for p in ro.product.device ro.product.model ro.build.version.release ro.board.platform ro.vendor.build.fingerprint; do
  raw "$p = $(getprop $p 2>/dev/null)"
done
raw "kernel = $(uname -a 2>/dev/null)"
# msm8953 sanity hint (not fatal - some ship as sdm/other names)
PLAT=$(getprop ro.board.platform 2>/dev/null)
log "platform: ${PLAT:-unknown}  (expected msm8953 / sdm* for this target)"

# ---- 2. relax gates so tracing can be configured --------------------------
SE_WAS=$(getenforce 2>/dev/null)
setenforce 0 2>/dev/null
echo 0 > /proc/sys/kernel/kptr_restrict 2>/dev/null
echo 0 > /proc/sys/kernel/perf_event_paranoid 2>/dev/null

# ---- 3. locate tracefs ----------------------------------------------------
T=""
for d in /sys/kernel/tracing /sys/kernel/debug/tracing; do
  [ -w "$d/tracing_on" ] && { T="$d"; break; }
done
if [ -z "$T" ]; then
  mount -t tracefs nodev /sys/kernel/tracing 2>/dev/null
  mount -t debugfs nodev /sys/kernel/debug 2>/dev/null
  for d in /sys/kernel/tracing /sys/kernel/debug/tracing; do
    [ -w "$d/tracing_on" ] && { T="$d"; break; }
  done
fi
[ -z "$T" ] && { log "!! FAIL: no writable tracefs (kernel may lack ftrace). Send this file anyway."; exit 1; }
log "tracefs: $T"

# ---- 4. find apr_send_pkt (the function that talks to the DSP) ------------
SYM=$(grep -iE ' (apr_send_pkt|__apr_send_pkt|gpr_send_pkt)$' /proc/kallsyms 2>/dev/null | head -1)
NAME=$(echo "$SYM" | awk '{print $3}')
ADDR=$(echo "$SYM" | awk '{print $1}')
log "apr symbol: ${NAME:-<none>} @ ${ADDR:-<none>}"

# arm64 uses x1 for the 2nd arg (the packet); arm32 uses r1
case "$(uname -m 2>/dev/null)" in
  aarch64|arm64) A=x1 ;;
  *) A=r1 ;;
esac

# ---- 5. install the kprobe ------------------------------------------------
echo 0 > "$T/tracing_on" 2>/dev/null
echo > "$T/trace" 2>/dev/null
echo nop > "$T/current_tracer" 2>/dev/null
echo 16384 > "$T/buffer_size_kb" 2>/dev/null
echo > "$T/kprobe_events" 2>/dev/null

# APR header: pkt_size@+2(u16), dest_port@+10(u16), token@+12, opcode@+16, payload@+20..
PROBE_BODY="op=+16(%$A):x32 sz=+2(%$A):u16 dport=+10(%$A):u16 tok=+12(%$A):x32 p1=+20(%$A):x64 p2=+28(%$A):x64 p3=+36(%$A):x64 p4=+44(%$A):x64 p5=+52(%$A):x64 p6=+60(%$A):x64 p7=+68(%$A):x64 p8=+76(%$A):x64"
OK=0
if [ -n "$NAME" ]; then
  echo "p:spk $NAME $PROBE_BODY" > "$T/kprobe_events" 2>>"$OUT" && OK=1
fi
if [ "$OK" != 1 ] && [ -n "$ADDR" ]; then
  echo "p:spk 0x$ADDR $PROBE_BODY" > "$T/kprobe_events" 2>>"$OUT" && OK=1
fi
if [ "$OK" != 1 ]; then
  log "!! kprobe on apr_send_pkt FAILED. Trying built-in AFE trace events as fallback..."
  # last-resort: enable any afe/adsp/apr trace events this kernel exposes
  FB=0
  for ev in $(ls "$T/events" 2>/dev/null | grep -iE 'afe|q6|apr|adsp|asoc'); do
    echo 1 > "$T/events/$ev/enable" 2>/dev/null && FB=1
  done
  [ "$FB" = 1 ] && log "fallback trace events enabled (less ideal but usable)" \
                || { log "!! FAIL: could not set up any tracing. Send this file so it can be diagnosed."; exit 1; }
fi
echo 1 > "$T/events/kprobes/spk/enable" 2>/dev/null
echo 1 > "$T/tracing_on" 2>/dev/null

# ---- 6. capture window ----------------------------------------------------
log ""
log ">>> NOW: from SILENCE, play sound ON THE LOUDSPEAKER for ~8 seconds."
log ">>>   (a song/ringtone through the speaker - NOT headphones, NOT earpiece)."
log ">>>   Make sure it is coming out of the speaker. Starting capture in 3s..."
sleep 3
echo > "$T/trace" 2>/dev/null       # clear anything from before playback
log ">>> CAPTURING for 8 seconds - play the speaker NOW..."
sleep 8
echo 0 > "$T/tracing_on" 2>/dev/null

# ---- 7. save the trace ----------------------------------------------------
raw ""
raw "================= APR TRACE (speaker playback) ================="
cat "$T/trace" >> "$OUT" 2>/dev/null
raw "================= END APR TRACE ================="

# ---- 8. supporting context for the diff -----------------------------------
raw ""; raw "--- mixer_paths / audio config files present ---"
for f in /vendor/etc/mixer_paths*.xml /system/etc/mixer_paths*.xml \
         /vendor/etc/audio_platform_info*.xml; do
  [ -f "$f" ] && { raw "### $f"; cat "$f" 2>/dev/null; }
done
raw ""; raw "--- ADSP firmware / ACDB files (names+sizes only) ---"
ls -l /vendor/firmware*/adsp* /vendor/firmware*/acdb* /vendor/etc/acdbdata/* \
      /firmware/image/adsp* 2>/dev/null >> "$OUT"
raw ""; raw "--- kernel audio log ---"
dmesg 2>/dev/null | grep -iE 'afe|q6afe|apr|adsp|tas256|smartamp|mi2s' | tail -60 >> "$OUT"
raw ""; raw "--- active PCM/mixer state ---"
cat /proc/asound/cards 2>/dev/null >> "$OUT"

# ---- 9. self-check: did we actually capture the AFE/speaker traffic? -------
NP=$(grep -c 'spk:' "$OUT" 2>/dev/null)
# AFE opcodes of interest: 0x100e5 START, 0x100ef SET_PARAM_V2, 0x100f3 SVC_SET_PARAM,
# 0x100e4 STOP, 0x1016/0x1017 quinary port ids seen anywhere in payload.
HIT=$(grep -ciE '0x100e5|0x100ef|0x100f3|0x0*1016|0x0*1017' "$OUT" 2>/dev/null)

# ---- 10. restore gates ----------------------------------------------------
echo 0 > "$T/events/kprobes/spk/enable" 2>/dev/null
echo > "$T/kprobe_events" 2>/dev/null
echo 0 > "$T/tracing_on" 2>/dev/null
[ "$SE_WAS" = "Enforcing" ] && setenforce 1 2>/dev/null

log ""
log "=========================================================="
if [ "${NP:-0}" -gt 20 ] && [ "${HIT:-0}" -gt 0 ]; then
  log "RESULT: PASS  (captured $NP DSP packets, $HIT AFE/speaker hits)"
elif [ "${NP:-0}" -gt 20 ]; then
  log "RESULT: PARTIAL  ($NP DSP packets, but no obvious AFE/speaker opcode)."
  log "        Re-run and make sure the sound really plays THROUGH THE SPEAKER."
else
  log "RESULT: FAIL/EMPTY  (only ${NP:-0} packets)."
  log "        Likely the speaker did not play during the window, or tracing was blocked."
  log "        Send the file anyway - the header explains what to fix."
fi
log "SAVED: $OUT"
log "Send that ONE file back. (You can copy it off with: adb pull $OUT )"
log "=========================================================="
rm -rf "$TMP" 2>/dev/null
