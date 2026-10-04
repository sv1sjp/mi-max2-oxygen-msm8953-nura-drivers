# Mi Max 2 (oxygen) audio: handoff

Written so work can resume from zero context. Camera work and the install history are documented in the main [README](../../README.md) (not part of this repository: the original working notes).
Last updated: 2026-09-29 (session 3: ADSP firmware RE).

## ▶ RESUME HERE  (consolidated 2026-09-30, after ~29 test boots)
**UPDATE 2026-09-30 (session 5, offline RE + 1 reboot-free test):** decoded the DMA↔interface
routing block from the ADSP firmware (see QUINARY-DRIVER-DESIGN.md "routing block DECODED"):
base 0x0c0cf000, routing regs 0x0c0d0004/0008/000c, RDMA 0x0c0d2000+ch*0x1000. Built+ran
`quinroute/quinroute.ko` (normal OS, no reboot): the three firmware mux patterns written to
**0x0c0d000c did NOT bind the DMA to Quinary** (DMA fills one FIFO then stalls, no pad data).
Finding: the read-DMA→interface routing for playback is in **0x0c0d0008** (=0x08008001 live
during working earpiece), not 0x0c0d000c. Next task narrowed: RE the 0x0c0d0008 setter encoding.
The Android APR capture remains the non-bypass alternative. Full detail in QUINARY-DRIVER-DESIGN.md.

**Goal:** loudspeaker sound on the Mi Max 2 (oxygen). Bottom speaker = TI **TAS2560** on **Quinary MI2S** (interface slot 5, pri_mi2s pads gpio88/91/92/93, amp DATA on SD2/SD3 = gpio94/95). Earpiece = 2nd TAS2560 (untouched). Amp is I2C-controlled by our `tas2560/tas2560.c` codec driver.

**THE PROBLEM:** starting the Quinary AFE port (0x1016) via the mainline DSP path (q6afe) **hangs** — `AFE enable for port 0x1016 failed -110`, DSP AFE dead until reboot. Quaternary/Primary start fine. During the hang the DSP sets up the Quinary bit clock but **never writes any I2S interface register** (test #26).

**ROOT CAUSE: NOT cleanly identified.** Evidence conflicts (be honest about this):
- Quinary hardware is FINE: `quinws/quinws.ko` drove the Quinary clock+interface directly from Linux and produced BCLK(gpio91)+WS(gpio92) on the amp pads, ADSP stayed alive.
- 3 firmwares all hang oxygen's Quinary: oxygen(has SmartAmp), vince(no SmartAmp), daisy(no SmartAmp, PROVEN-working Quinary on Mi A2 Lite). BUT vince/daisy fw tests are CONFOUNDED (different ADSP versions likely need their own ACDB), so "board-specific" is not proven.
- Mi A2 Lite (daisy) speaker WORKS on the SAME mainline kernel with near-identical audio DT (Quinary, MAX98927, pri pins). No LPASS/audio regulator/clock difference vs oxygen in the board DTs. So the hang is NOT in the audio driver or audio DT.
- Candidate causes still open: (a) SmartAmp in oxygen's OWN fw hanging at its MMPM power-vote / dynamic-thread step (before the tas2560 bin load — hexagonrpcd saw no file request); (b) a board/boot precondition (Fairphone-3-class, PIL-vs-PAS).

**TWO VIABLE PATHS FORWARD:**
1. **Linux Quinary bypass — FEASIBILITY PROVEN, needs finishing.** `quindma/quindma.ko` proved Linux CAN drive the LPAIF read-DMA (CURR advances) — TrustZone does NOT lock LPASS on this unlocked phone. Remaining: decode the DMA↔interface routing (it's NOT in the DMA CTL; it's an undocumented block near 0x0c0d0000 — see "Correction + next task" below), bind a read-DMA channel → Quinary slot5, set I2S_CTL5 SPKMODE for SD2/SD3, sample gpio94/95, then build it into a small lpass-cpu-style driver + UCM. Real multi-session work but a KNOWN path.
2. **Android APR capture — could reveal the proper q6afe fix (no bypass).** See `ANDROID-APR-CAPTURE.md`. Boot the Mi Max 2 (or any working msm8953) to Android, ftrace `apr_send_pkt` while the loudspeaker plays, send me the trace. Decisive but needs an Android boot (deferred — user can't now). This is the method that cracked Fairphone 3/5.

**IMPORTANT working rule:** the user dislikes reboot/guess loops — ask before every test boot, prefer offline analysis, batch what a boot answers. See memory `no-reboot-loops`.

**Key tools built this project (all in `audio-research/`):** `quinws/` (drive Quinary interface from Linux — proven), `quindma/` (drive LPAIF DMA — proven), `padsample/` (sample TLMM pad activity), `regsnap/` (snapshot MMIO ranges to debugfs), `lpassmux/` (poke LPASS mux regs), `qdsp6ss-bit/` (FP3 register, not our fix), `tas2560/` (amp codec driver), `q6afe-topo/`,`apq8016-sbc/` (patched q6afe/machine modules), `scripts/` (testboot.sh, mkramboot.sh, an SSH helper that is not part of this repository). Firmware: `adsp-fw/` (oxygen ADSP disasm), `donor-fw/{vince,daisy}/adsp/`. Design: `QUINARY-DRIVER-DESIGN.md`.

**Historical note:** the "SmartAmp is the root cause" framing below (session 3) is NOT confirmed — vince fw has zero SmartAmp yet also hung. Kept for the RE detail. The stereo/4-ch and disable-param experiments below were superseded.

### ADSP firmware RE results (session 3, 2026-09-29)
Work dir `adsp-fw/`: `adsp.elf` (rebuilt), `segNN.o` (raw segments wrapped as ELF), `disNN.txt` (llvm-objdump Hexagon), **`a04.txt`** = segment b04 disassembly annotated with strings + debug-message structs (`annot.py`), `cg.py <addr> <depth>` = rough call-graph walker. Alpine's llvm has the Hexagon target (container `hex`).
- Debug message struct = `{u16 line, u16 ssid(0x2134=QDSP6), u32 mask, char *fmt}` at 0xf07b9f00…0xf07baf60, so every log call maps to a code address + source line.
- **SmartAmp is auto-enabled by the ADSP itself:** in `afe_port_apr_msg_handler`, opcode 0x100E5 (DEVICE_START) with port 0x1016/0x1017 → `SmartAmp_enable` (0xf0553e7c) unconditionally, then the normal port start (0xf054cf88). Linux cannot avoid it by *not* sending a param, and experiment 2 (disable param) could only work if sent before START (enable only accepts CONFIG/STOP state).
- `SmartAmp_enable`: alloc state (port+0x2f4), MMPM vote (0xf054b77c, generic AFEMmpm code), set enabled=1. MMPM failure → "disabling the module" (non-fatal).
- Port start → if enabled → `SmartAmp_init` (0xf0553f38): lib-size query, **bin load** (0xf0553674: `fopen("tas2560_ssi.bin"/"tas2560_aac.bin","a+b")` via the libc → `ADSP_LIBRARY_PATH`/`apps_std` FastRPC path, i.e. the hexagonrpcd listener), buffer allocs, sets bits in global mask 0xf0cf87cc (bit2 = RX 0x1016, bit1 = TX 0x1017), launches a dynamic thread "AfeDy%2lx" (0xf0549a68).
- Processing callbacks (0xf0553318 TX, 0xf05533ec RX) only run the algorithm when the mask == 6 (**both RX and TX** active). RX needs 1–2 ch ("not stereo mode" otherwise), TX 2 or 4 ch. Nothing there blocks.
- Since hexagonrpcd saw no file request, the hang is most likely **before the bin load** (MMPM vote / alloc / lib-size) or in the thread launch. Not yet pinned down.
- **File access paths:** the rcinit task `rfsa_client` (entry 0xf01c92a4) actually sets up the RFS-over-TFTP layer → `tqftpserv` (the ADSP does `WRQ /readwrite/server_check.txt` every boot). The persistent journal shows **no other tqftpserv request in any boot, including all Quinary hang boots**, and hexagonrpcd (apps_std) saw none either ⇒ the hang is **before the tas2560 bin load**: in the port-start steps before `SmartAmp_init`, or in the enable/MMPM step.
- MMPM vote 0xf054b77c is the generic AFEMmpm function used by every port start (event bits 1…0x400; 0x100/0x200 = SmartAmp RX/TX MIPS). An unlikely hang point, but not ruled out.
- Xiaomi's kernel only uses the SmartAmp module for calibration get/set (`tas_calib`), so Android sends nothing special before START. The difference must be elsewhere (runtime state, ACDB/topology, voting…).
- No TDM ports (0x90xx) in this firmware, so bypassing the 0x1016/0x1017 hook via Quinary TDM is not possible.
- ⇒ Static RE has hit diminishing returns without runtime visibility. **The Primary-MI2S workaround (fallback 4) avoids SmartAmp entirely**, and its only failure was "no sound" with the amp on and no clock errors. That points at amp-side config (ASI format/slot, mute, DAC/boost), which is debuggable without the ADSP. Suggested next: redo it with both amps, check TAS2560 ASI format regs (I2S vs left-justified, data delay, slot/channel), DAC mute and page-0 status regs while playing.
- ADSP diag: `rpmsg_char` can be bound to `remoteproc2:smd-edge.DIAG` / `DIAG_CNTL` via `driver_override` (no rpmsg_ctrl in the kernel). The control handshake works (feature mask, cmd registrations, SSID ranges), but no F3 messages were received yet (`scripts/adsp-diag.py`, experimental).

### Next experiments, in priority order
1. **Stereo / 4-channel hypothesis** (user hint + firmware string `SmartAmp: not stereo mode`).
   Android always plays through **both** amps (mixer path `speaker` = `SPK Mixer SPK` + `RCV Mixer RCV`), RX 2 ch, and the feedback TX is **4 channels** (`msm_quin_mi2s_tx_ch = 4`, tx-lines 0x03 = SD0+SD1 → QUAD01). All tests so far used one amp and RX-only or TX 2 ch.
   Try:
   - DT: add amp **0x4f** (`receiver_amp`, reset gpio127, `ti,asi-channel = <0>` left) next to 0x4c (`ti,asi-channel = <1>` right). Put **both** as codecs in the Quinary RX link (multi-codec: `sound-dai = <&speaker_amp>, <&receiver_amp>;`) and in the TX link.
   - Patch the machine driver (`apq8016-sbc/`, it already builds as `snd-soc-apq8016-sbc-test` + `modprobe.blacklist=snd_soc_apq8016_sbc`): make the BE fixup give QUINARY_MI2S_TX **4 channels** (fixup currently forces 2 ch for all BEs), and maybe S24/32-bit (Android speaker bit width = 24).
   - Enable IV-sense output on the amps (TAS2560 ASI TX config; see the Xiaomi driver / datasheet).
   - Start **TX capture and RX playback together** (e.g. `arecord -D hw:0,1 -c 4 … & aplay -D hw:0,2 …`).
2. **Disable SmartAmp through a DSP param before port start.** Strings: `Smartamp en/disable=%d failed, Accept in CONFIG/STOP state only`, `portEnable != SMARTAMP_ENABLE`, `Smartamp: port = %x, Enable/Disable status = %d`.
   Module `AFE_SMARTAMP_MODULE 0x0F010209`, param `AFE_PARAM_ID_SMARTAMP_DEFAULT 0x10001166` (Xiaomi `smart_amp.h`); the algo-ctrl param id is `index | (len << 16) | (slave << 24)`. The exact enable param/index needs RE (step 3).
   Send it with a q6afe patch (`q6afe-topo/` already has the hook point in `q6afe_port_start`, loaded as `q6afe_topo` with `module_blacklist=q6afe`).
3. **Reverse-engineer the ADSP firmware.**
   - Rebuild the ELF from `adsp.mdt` + `adsp.b00..b13` (phone: `/run/msm-firmware-loader/mnt/modem/image/`). The mdt holds the ELF + program headers, and bNN are the segments.
   - Disassemble with `llvm-objdump --triple=hexagon` (check the Alpine llvm has the Hexagon target; otherwise Ghidra + a Hexagon plugin).
   - Find xrefs to the `AFESmartamp.cpp:` strings (all listed in `adsp-smartamp-strings.txt`; SmartAmp code is in segment b07). Follow the port-start path (`SmartAmp: AFE_PORT_STATE_RUN port id is %x`, `Smartamp init port id is %x`) to see what it waits for (param? feedback port? MMPM vote? dynamic thread?) and which param id enables/disables it.
4. Fallbacks: the Primary-MI2S workaround ran without DSP errors, but gave **no sound on SD0–SD3** (see log). Maybe revisit with both amps / other ASI settings.

### Test rules (important)
- **Always return the phone to the normal OS after a test boot** and verify `/proc/cmdline` starts with `quiet splash` and `/dev/dri/card0` exists. Twice the phone was left in a test image = black screen for the user. Use `scripts/testboot.sh <img> '<cmds>'`, which always restores.
- A Quinary hang wedges the ADSP until reboot. One Quinary attempt per boot; put the other checks before it.
- Keep test volume low: `-20 dBFS` tone + `Speaker Amp Gain Volume` 0–6 dB. Android's SmartAmp normally protects these speakers.
- If the user needs to listen: tell them it happens about 3–4 min after the screen goes black, and use beep patterns (`scripts/beeps-N.wav`) so they can report which variant they heard.

## How to run things
- SSH: `ssh user@172.16.42.1 '<cmd>'` over USB networking (user `user`, password `<password>`; sudo via `echo <password> | sudo -S -p "" …`).
- Build a module against the running kernel (an out-of-tree build against the running kernel's exact configuration, see `kernel/build-modules.sh`):
  ```sh
  docker run --rm -u 0 -v "<workdir>:/m" alpine:3.24 sh -c 'apk add -q clang lld llvm make bison flex perl python3 openssl-dev linux-headers musl-dev binutils gcc elfutils-dev; cd /m/kernel/linux-7.0.9-r0 && make ARCH=arm64 LLVM=1 O=/m/build/obj M=/m/audio-research/<dir> modules'
  ```
  For modules copied from the tree with relative includes, add `ccflags-y += -I$(srctree)/sound/soc/qcom[/qdsp6]` (see `apq8016-sbc/Makefile`, `q6afe-topo/Makefile`).
- DTB: the DTS files are in `kernel/dts/` and `audio/dts/`, compiled by `kernel/build-modules.sh` (alpine container with `gcc` for cpp, and the kernel's `dtc`).
- RAM-boot image: `scripts/mkramboot.sh <dtb> <out.img> ["module_blacklist=q6afe"]`. Run it in `scripts/`, which holds `vmlinuz` + `initramfs` copied from the phone's `/boot`.
- **Replacing an in-tree module in a test boot:** add `module_blacklist=<name>` (hard block) or `modprobe.blacklist=<name>` to the cmdline, then insmod the patched copy under a different name and insmod the dependents by path. `scripts/topo-test.sh` does this for q6afe:
  `q6afe_topo.ko`, then `q6afe-clocks q6afe-dai q6adm q6routing q6asm-dai q6voice q6voice-dai` from `/lib/modules/$(uname -r)/kernel/sound/soc/qcom/qdsp6/`, then `modprobe snd_soc_apq8016_sbc`, then `insmod ~/tas2560.ko`.
- Playback routes: Quinary `amixer -c0 cset "name=QUIN_MI2S_RX Audio Mixer MultiMedia3" 1` + `aplay -D hw:0,2`. Primary `PRI_MI2S_RX Audio Mixer MultiMedia1` + `hw:0,0`. Capture MM2 = `hw:0,1` (`MultiMedia2 Mixer QUIN_MI2S_TX`).
- Read amp registers while playing (page 0): `i2ctransfer -f -y 0 w2@0x4c 0x00 0x00; i2ctransfer -f -y 0 w1@0x4c <reg> r1`. Reg 7 = PWR (0x40 on, 0x01 shutdown); regs 38/39 = FLAGS (clock/OC/UV errors).

## Hardware
| | Earpiece amp ("RCV", left) | Loudspeaker amp ("SPK", right) |
|---|---|---|
| Chip | TI TAS2560 (no on-chip DSP) | TI TAS2560 |
| I2C | **i2c-0** (`&i2c_2`, `i2c@78b6000`), **0x4f** ✅ | **0x4c** ✅ |
| Reset GPIO | **127** (active low) | **20** |
| IRQ GPIO | 63 | 21 |
| Load | 8 Ω table | 8 Ω (`ti,right-load-aac/ssi = <0>`) |
- Downstream: `qcom,msm-ext-pa = "quinary"`; quin **rx-lines 0x0c (SD2+SD3)**, **tx-lines 0x03 (SD0+SD1)**.
- On msm8953 Quinary is muxed onto the **primary MI2S pins**: gpio88/91/93 (func1) + 94/95 (func2) `pri_mi2s`, gpio92 `pri_mi2s_ws`. The `quin-iomux` register (0xc052000) bit0 = route Quinary to those pins. On vince: gpio91 = SCK, gpio92 = WS, gpio88/93 = data.
- Speaker ID: gpio9 three-state (`spk_id`): pull-down = SSI, float = AAC. It selects `tas2560_ssi.bin` / `tas2560_aac.bin` (SmartAmp tuning). The DSP param `AFE_SA_SET_SPKID` exists.
- Android (vendor `mixer_paths_mtp.xml`): `speaker` = SPK+RCV mixers (stereo); `handset` = RCV only; `vi-feedback` path is empty; Speaker device bit width 24; ACDB id 34 (= "HANDSET_MIC_STEREO" in QRD ACDB, so the ACDB mapping is odd). `TAS2560 Speaker/Receiver DAC Volume 14db`.

## What's installed on the phone (normal OS)
- UCM `xiaomi-oxygen` (`ucm/` here) → `/usr/share/alsa/ucm2/conf.d/xiaomi-oxygen/` + `Xiaomi/oxygen/HiFi.conf`. Devices: Earpiece (WCD EAR, probably not wired), Headphones, Mic1/2, Headset. No Speaker yet.
- **PulseAudio** is the audio server (`pulseaudio -k` to restart; PipeWire audio disabled by `51-pulseaudio.conf`).
- `hexagonrpcd` + `strace` packages. hexagonrpcd units **disabled**. Vendor DSP files copied to `/usr/share/qcom/msm8953/Xiaomi/oxygen/` (`dsp/adsp/`, `vendor/lib/rfsa/adsp/`, `acdb/`).
- In `~`: `tas2560.ko`, `fastrpc.ko`, `q6afe_topo.ko`, `snd-soc-apq8016-sbc-test.ko`, `regdump.ko`, `dw9768.ko`, `imx386.ko`, `topo-test.sh`, `tone.wav`, `beeps-1..4.wav`.
- Nothing audio-related in `/boot` or `/lib/modules` (normal boot is unchanged apart from the UCM).

## Code written
- `tas2560/tas2560.c`: new mainline ASoC codec driver (from Xiaomi's source).
  - Paged regmap (reg = page*128 + reg, selector reg 0); reset GPIO + SW reset; DR_BOOST 0x04, DEV_MODE 0x02 at probe.
  - `hw_params`: word length, SR tables, PLL from BCLK (1.536 MHz → P=1 J=32 D=0). DAPM ClassD POST_PMU = Xiaomi power-up (HPF, load, boost headroom, thermal foldback, Vsense biquad, SR, CLK_ERR 0x0b, INT_GEN 0xff, PWR 0x40), PRE_PMD = power-down.
  - DT: `ti,load-ohms` 4/6/8, `ti,asi-channel` 0 L / 1 R / 2 (L+R)/2 / 3 mono. Controls "Amp Gain Volume" (0–15 dB), "ASI Channel". Playback + capture (IV) streams.
  - All 172 coefficient bytes verified against Xiaomi by script.
- DT test files (in the kernel dts dir):
  - `msm8953-xiaomi-oxygen-audio.dts`: includes the camera+AF test DT. Amp 0x4c on `&i2c_2`; Quinary RX link (`dai@127 sd-lines <2>`) + TX link (`dai@128 sd-lines <0 1>`); `tlmm_pri_ws_default`; **FastRPC node** on the ADSP smd-edge.
  - `-audio-noprim.dts`: no PRI/TER links.
  - `-audio-sectest.dts`: Secondary MI2S test link (starts OK).
  - `-audio-prishare.dts`: amp on the Primary link, `dai@16 sd-lines <2>`, Quinary removed.
- Patched modules: `apq8016-sbc/` (param `quin_legacy_clk`: v1 LPAIF clock for Quinary), `q6afe-topo/` (params `quin_none_topology`, `pri_sd_mask`), `fastrpc/` (CONFIG_QCOM_FASTRPC isn't set in pmOS), `regdump/` (prints the LPASS iomux registers).
- Images here: `audio-ramboot.img` (Quinary + FastRPC), `audio-noprim-ramboot.img`, `audio-v1clk-ramboot.img` (`modprobe.blacklist=snd_soc_apq8016_sbc`), `audio-topo-ramboot.img` / `audio-prishare-sweep-ramboot.img` (`module_blacklist=q6afe`), `audio-sectest-ramboot.img`, `audio-prishare-ramboot.img`.

## Experiment log (all 2026-09-29)
Every Quinary attempt: `AFE enable for port 0x1016 failed -110` at prepare. Only one such line is logged, so the I2S config param is accepted and **`AFE_PORT_CMD_DEVICE_START` hangs**. Afterwards the whole ADSP is dead (AFE and ADM time out).
Ruled out, each by a test boot:
- sd-lines `<0>`, `<2>`, `<2 3>` (mainline maps QUAD23 → SD2 for 2 ch, like downstream).
- Quinary TX alone (0x1017, `<0 1>`, 2 ch) also hangs.
- Clocks: IBIT v2 (id 0x10B, COUPLE_NO, root 0, 1.536 MHz, same as downstream) and v1 `LPAIF_BIT_CLK`: both hang. All AFE constants/structs are identical to Xiaomi `apr_audio-v2.h`.
- Explicit no-processing AFE topology (`AFE_PARAM_ID_SET_TOPOLOGY` 0x1025A, RX 0x112FC / TX 0x112FB, what the ACDB uses for every device): the DSP accepts it, still hangs.
- A running Primary stream at the same time (LPASS core clocks on): hangs. Removing the PRI/TER links: hangs.
- Pins/iomux: correct (regdump: mic 0x00200002, spkr 0x00030000, quin 0x00000001, pri 0x00000000).
- Firmware = Xiaomi's own (modem partition via msm-firmware-loader).
- Android userspace: no vendor program uses tas2560/tas_calib. `adsp_avs_config.acdb` only registers Dirac/aptX. `/dsp` has only decoders/post-proc. The 4.9 LineageOS kernel (`lineage-23.2/`) does the same as Xiaomi 3.18.
- **FastRPC + hexagonrpcd** (serves ADSP file requests): works (the ADSP requests `remote_heap_config.so` at attach), but under strace the ADSP makes **no** file request at Quinary start. So SmartAmp hangs before loading `tas2560_*.bin`.
- **Secondary MI2S RX starts fine. Primary works.** Only Quinary hangs.
- **Primary-MI2S workaround** (amp on the Primary link, quin iomux 0): runs with no DSP errors; the amp is on (PWR 0x40) with no clock errors. But **no sound on SD0, SD1, SD2 or SD3** (swept in one boot with `pri_sd_mask`, user listened, heard nothing).

## ADSP firmware facts
Build path `/home/work/oxygen-n-stable-build/vendor/qcom/non-hlos-msm8953-spf20/ADSP.8953.2.8.2/`. Debug strings are present. SmartAmp code is in `adsp.b07` (`AFESmartamp.cpp`, `AFEPortAprHandler.cpp:SmartAmp get port id is %x`). `adsp.b04` has `rfsa_client`, `apps_std`, `fopen`, `ADSP_LIBRARY_PATH`, `tas2560_aac.bin`, `tas2560_ssi.bin`. The full list is in `adsp-smartamp-strings.txt`.
Notable strings:
- `SmartAmp: AFE_PORT_STATE_RUN port id is %x`
- `Smartamp init port id is %x`
- `SmartAmp: not stereo mode`
- `portEnable != SMARTAMP_ENABLE`
- `Smartamp en/disable=%d failed, Accept in CONFIG/STOP state only`
- `MMPM voting failed while enabling SmartAMP module`
- `Failed to launch dynamic thread for port`
- `SmartAmp: failed to open bin file`
- `binfile parsing, already done! SpkId = %d`
- `AFE_SA_SET_SPKID / SPK_RE / RCV_RE`
- profiles: MUSIC / VOICE_HANDSET / VOICE_HANDSFREE / RINGTONE / MOVIE-GAME / FCT
- `SmartAmp: deInit RX/TX`
- `capi_v2_sp_v2_*_vi … Feedback info not yet rcvd!` (Qualcomm speaker-protection modules also present)

## References in this directory
- `xiaomi-oxygen-n-oss/`: Xiaomi 3.18 kernel sparse checkout (`sound/soc/codecs/tas2560*`, `sound/soc/msm/msm8952*.c`, `sound/soc/msm/qdsp6v2/{q6afe,msm-dai-q6-v2}.c`, `include/sound/{apr_audio-v2,q6afe-v2,smart_amp}.h`, `drivers/tas_calib/`).
- `lineage-23.2/`: files from github.com/Deeping415/android_kernel_xiaomi_oxygen (branch lineage-23.2): `oxygen/audio.dtsi`, pinctrl, `techpack/audio/{asoc/msm8952.c,asoc/msm-dai-q6-v2.c,dsp/q6afe.c}`.
- `android-vendor/`: Android vendor audio files (`etc/mixer_paths_mtp.xml`, `etc/audio_platform_info_intcodec.xml`, `etc/acdbdata/*`, `lib/rfsa/adsp/tas2560_{aac,ssi}.bin`, `lib/hw/audio.primary.msm8953.so`, `lib/libacdbloader.so`).
  - ACDB format: `QCMSNDDB` + chunks (8-char tag, u32 len, data); `DPROPLUT` = u32 n + n×{dev, pid, datapool offset}; `DATAPOOL` entries = u32 len + data. pid 0x113b8 = name (UTF-16), pid 0x13150 = AFE topology.
- `vince-ref/`: github.com/kotXio/postmarketos-xiaomi-vince (working TAS2557 on Quinary, `sd-lines <0>`; also DW9763 focus, OV12A10). Mainline TAS2557 driver thread by Gianluca Boiano: https://ratatoskr.run/linux-sound/2026/07/17277306/t
- `scripts/`: `testboot.sh`, `mkramboot.sh`, `topo-test.sh`, tones, `vmlinuz`/`initramfs` (from the phone's `/boot`).
- Vendor partition read-only mapping (phone): `sudo dmsetup create --readonly android_vendor --table "0 917528 linear /dev/mmcblk0p65 2269856"; sudo mount -t ext4 -o ro,noload /dev/mapper/android_vendor /mnt/sys` … then `umount` and `dmsetup remove android_vendor`.

## Session 3 (2026-09-29): donor ADSP firmware test (IN PROGRESS)
- Other msm8953 pmOS phones (tissot, daisy, vince, asus ze5xx, moto ocean) run Quinary MI2S fine on mainline, so the only difference is the ADSP firmware.
- **vince (Redmi 5 Plus) ADSP firmware** (`donor-fw/vince/nonhlos/image/`, from XiaomiFirmwareUpdater fw_vince V11.0.2.0 NON-HLOS.bin) is signed by the same Xiaomi Root CA 1 chain, has the same HW/OEM/MODEL IDs and **no SmartAmp**. **It authenticates and boots on oxygen** (tested in the normal OS). Copy on the phone: `~/vince-adsp/`.
- ⚠ **Never stop the ADSP while vince firmware is running**: it hung the phone (storage/cpufreq D-state, needed a sysrq reboot). Swap once, then reboot.
- Test: `scripts/vince-quin-test.sh` (on the phone as `~/vince-quin-test.sh`) run via `scripts/testboot.sh audio-ramboot.img 'echo <password> | sudo -S -p "" /home/user/vince-quin-test.sh'`. Before stopping the ADSP, unbind `c0f0000.codec` (msm8916-wcd-digital-codec) and rebind it after the swap, otherwise the card fails with `Failed to set mclk: -22`.
- Run #3 (logs `scripts/vince-quin-test3.log`): the card came up, but the log shows `sh: write error: Invalid argument` and `adsp: offline` after the swap, so the ADSP start may have failed that run. Check the log: did Quinary playback give -110 or sound? If the ADSP was offline, add a retry/longer wait for `start`.
- Run #3 result: `remoteproc remoteproc2: Boot failed: -22` for vince/adsp.mdt **in the test-boot image** (the same firmware booted fine in the normal OS). The Quinary -110 afterwards is only from the stale APR devices, so it is NOT a real test. Next: compare the ADSP reserved-memory / remoteproc node of `msm8953-xiaomi-oxygen-audio.dtb` vs the normal DTB (vince's segments are bigger, e.g. b12 85 KB vs 23 KB), check the full dmesg around the -22, or do the swap in the normal OS with the Quinary test DTB changes applied differently.
- **Runs #4–5: SmartAmp is NOT the (only) cause.** With the ADSP booted directly on vince's firmware (`audio-vince-ramboot.img`, DTB `msm8953-xiaomi-oxygen-audio-vince.dts` = audio DT + `&lpass { firmware-name = "xiaomi-vince-adsp/adsp.mdt"; }`, files in `/usr/lib/firmware/xiaomi-vince-adsp/` on the phone; test `scripts/vince-boot-test.sh`), Quinary START **still times out** (`AFE enable for port 0x1016 failed -110`, AFE dead afterwards). The same firmware + mainline stack works on vince ⇒ the difference is on the oxygen side (DT/config/pins).
- vince's config: Quinary **RX only, `sd-lines = <0>`**, no TX link, own pri_mi2s pinctrl states. Ours: sd-lines <2> + a TX link. `<0>` was only ever tested with oxygen's SmartAmp firmware. Next test: `audio-vince-rx0-ramboot.img` (vince fw + vince config).
- ⚠ Base oxygen DT bug: `tlmm_pri_act` and `tlmm_pri_sus` are both named `pri-tlmm-state`, so they merge. The active MI2S pin state is really 2 mA + pull-down. Fix by giving them distinct node names (e.g. `pri-tlmm-active-state` / `pri-tlmm-sleep-state`).
- Runtime ADSP swap lessons: stopping the original ADSP unregisters q6afe clocks held by the WCD digital codec (`Failed to set mclk: -22`, card fails). Unbinding the codec before stop makes the stop hang; rebinding after start works but gives a WARN, and DPCM then found no backend. Booting the donor firmware via DT `firmware-name` avoids all of it.
- **Runs #6–9:** vince fw + vince config (`audio-vince-rx0-ramboot.img`, RX only, sd-lines <0>) still gives `AFE enable for port 0x1016 failed -110` (run #8, confirmed). ⇒ same kernel (vince pins v7.0.9-r0), same ADSP firmware, same Quinary config as the working vince, yet Quinary START hangs on oxygen.
- Checked and identical to vince/daisy/downstream: machine driver clock/iomux code, WS/SCK/data pins (88/91/93/94/95 + ws 92), ADSP reserved memory (0x8d600000/0x1200000), no GCC audio clocks. Downstream quinary startup = quin iomux bit + IBIT clock + pins only.
- **Flaky DPCM route:** `QUIN_MI2S_RX Audio Mixer MultiMedia3` can read "on" while the DAPM path is disconnected (`no backend DAIs enabled for MultiMedia3`). Cause: `q6routing` put() returns early when `session->port_id == be_id` (q6routing.c:500), likely stale across a card re-creation. `scripts/vince-boot-test.sh` now waits 10 s, toggles 0→1 and verifies `MM_DL3 → QUIN_MI2S_RX Audio Mixer` in debugfs before each aplay. (The updated script is local only and not yet uploaded to the phone.)
- Run #9 (`quin-min-ramboot.img`: plain oxygen DT + amp + vince-style Quinary + vince fw + fixed 8 mA pins, no camera/FastRPC): the route never connected, so no START was sent. Remoteproc numbering changes with this DT (remoteproc2 = wcnss), and the script now finds the ADSP by name. **After run #9 the phone hung during the restore reboot** (USB gadget 18d1:d001 present, no network, no fastboot) and needed a manual power-button reset.
- **Run #10: FP3 QDSP6SS bit tested, not the fix.** Our phone under PAS reads `0x0c20002c = 0x10b` (bit 3 set), the same as the Fairphone 3 SDM632 SLIMbus case (downstream PIL = 0x103). Module `qdsp6ss-bit/qdsp6ss_bit.ko` (`clear=1`) cleared it to 0x103 right before playback (route verified connected), but the Quinary START still gave `AFE enable for port 0x1016 failed -110`. (Caveat: the ADSP might re-set it between the clear and START. FP3 had to clear it inside the NGD power-up path.)
- Research so far: no public report of a Quinary START hang on msm8953 mainline. Working Quinary devices in-tree: tissot/daisy (MAX98927), vince (TAS2557, same kernel v7.0.9-r0), asus ze520kl/ze552kl (TFA9895), moto ocean (TFA9874). None needs extra kernel code.
- **Run #11:** the vince speaker UCM plays via **MultiMedia1 → PRI_MI2S_RX + QUIN_MI2S_RX (hw:0,0)**, not MM3. Replicated exactly (`scripts/vince-mm1-test.sh`, vince fw + vince config): Quinary START still `-110`. Afterwards Primary also times out. So concurrent Primary is not the missing precondition.
- Asked the user to search GitHub (logged in) for: UCMs using `QUIN_MI2S_RX Audio Mixer`, `"ti,tas2560"`, oxygen audio forks, msm8953-mainline issues about quinary/0x1016, and the FP3 `0x0c20002c` fix.

### Session 3 (evening): pad-level findings. READ THIS
- **Quaternary MI2S (0x1006) bypasses the SmartAmp hang**: with oxygen's own firmware, Quaternary START works on all SD lines, and the amp reports PWR=0x40 with FLAGS 0 (`quat-ramboot.img`, DT `msm8953-xiaomi-oxygen-quat.dts`, `scripts/quat-sweep-test.sh`, `q6afe-topo` param `quat_sd_mask`). **But no sound**: the pad sampler shows Quaternary drives NONE of the pri_mi2s pads 88–95 (it goes elsewhere, e.g. sec_mi2s 135–138 as on the Lenovo cd-18781y). The amp's clock-error flags are unreliable (0 even with no clock).
- **Pad sampler** `padsample/padsample.ko` (`first=N tag=..`): samples TLMM GPIO_IN of 8 pads 200k times. Validated on busy I2C pads gpio6/7 (ACTIVE). Pins: pri_mi2s = gpio88/91/93/94/95 (+66), pri_mi2s_ws = gpio92, sec_mi2s = 135–138.
- **Quinary START (vince fw, 8 mA pins, `quin-min-ramboot.img`, `scripts/quin-pads-test.sh`)**: during the hang **gpio91 (BCLK) toggles (50% duty) but gpio92 (WS) and all data pads stay flat**. BCLK comes from the IBIT clock request made before START, while WS comes from the interface itself ⇒ the DSP freezes before or while enabling the Quinary interface, not on pads.
- `lpassmux/lpassmux.ko` (addr/clr/set on the 4 LPASS mux regs): clearing spkr_iomux PRI_WS_SLAVE_SEL (0x30000→0, downstream never sets bit 17) did **not** help.
- FP3 (msm8953-mainline issue #255) has a working mainline Quinary speaker (aw8898) under PAS. Its bit-3 (0x0c20002c) problem is SLIMbus-only. That thread warns that binding ADSP DIAG_CNTL without the diag handshake can cause a SoC reset.
- **Runs #16–19 (all Quinary START, vince fw unless noted, pad-sampled):**
  - #16 full scan of every accessible pad (gpio4–134; TZ-reserved 0–3 and 135–138 skipped) during the hang: **only gpio91 (BCLK) toggles anywhere**, no WS or data on any pad. Baseline before START: nothing active.
  - #17 request `LPASS_CLK_ID_QUI_MI2S_OSR` (12.288 MHz) before START (q6afe-topo `quin_osr_hz`): the DSP rejects it (-22) and it still hangs.
  - #18 FP3-style card (`msm8953-xiaomi-oxygen-quin-fp3style.dts`: Quinary link only, Primary/Tertiary deleted, pins 88/91/93 + ws 92 only): still hangs.
  - #19 QDSP6SS 0x0c20002c bit 3 **held** clear during START (qdsp6ss_bit `hold_ms`): 90k checks, the ADSP never re-set it, and it still hangs. ⇒ the FP3 bit is definitively not it.
  - Normal-OS clock tree: `INTERNAL_DIGITAL_CODEC_CORE` is already enabled (9.6 MHz), so it's not a missing codec MCLK.
- **Conclusion so far:** on oxygen the Quinary MI2S interface never starts (WS never generated), while BCLK (the IBIT branch Linux enables first) runs. Same kernel, firmware and DT style work on vince/FP3/daisy. The cause is oxygen-specific SoC state (boot chain / TZ devcfg / LPASS), not Linux audio config.
- Next diagnostic ideas: dump LPASS-CC (0x0c000000, 80 KB) and the LPAIF block during a Quaternary run (works) vs a Quinary hang, and look for the Quinary interface CBCR / I2S_CTL state (was the enable even written? is a core branch CLK_OFF?). Or ask the vince/FP3/msm8953 people.

### Session 3 (late): DSP code reading + final tests
- **ADSP audio HW map (from firmware devcfg/clock tables, LPASS base 0x0c000000):** clock branches pri 0x0b014/18/1c, ter 0x0d0xx, quad 0x0e0xx, **qui 0x32014 (osr) / 0x32018 (ibit+cdiv) / 0x3201c (ebit)**, sen 0x33xxx, digcodec 0x2c014, mclk0/1 0x34014/0x35014. LPAIF block 0x0c0c0000 (+0x20000), **I2S_CTL[n] = 0x0c0c4000 + n*0x1000** (7 slots, devcfg interface list 0,1,2,3,5,6). DMA sub-blocks 0x0c0cf000/0x0c0d2000/0x0c0d8000. Mux CSR block 0x0c050000.
- **I2S HAL (V1, function table @0xf0cf0728):** init f0539868, config f05397c8 (I2S_CTL: SD mode bits 10–13, mono bit 9, WS src bit 2, bit width 0–1, mask 0x8007|0x3e00), **enable f0539794 = set bit 14 (0x4000)** for RX / 0x100 for TX, disable f0539764, idx→irq map f053971c. All go through the RMW helper f05476b4. **No wait loops** in the I2S/DMA HAL.
- ADSPPM clock enable `halHwIo_EnableCgcClock` (f019c26c) has an **unbounded status-poll loop** at f019c32c (for HWIO clocks with a wait mask). Not tied to Quinary yet.
- Snapshot (#24): during Quaternary playback I2S_CTL[3] (0x0c0c7000) = 0x000f4400, others 0x000f0004. Reading the whole LPAIF window while it was active killed the ADSP (don't do that again; read single known registers only).
- **#25:** mux registers set to the exact downstream Android state (spkr 0x00010000, mic 0x00200000, quin 0x1) before START: **still only BCLK, no WS, -110**.
- **Status:** every software/config difference between mainline and the working downstream kernel/firmware that is visible from Linux has been tested. The Quinary interface never starts (no WS) on oxygen, while the identical setup works on vince/FP3. Remaining explanation: oxygen-specific boot-chain/TZ/LPASS state that PIL establishes and PAS does not, which is not visible from the AP.

### Path forward: Linux-driven Quinary (DSP bypass)
Proven with `quinws/quinws.ko` (normal OS, no reboot): Linux driving the Quinary bit clock +
quin iomux + `I2S_CTL[slot 5]=0x000f4400` produces BCLK (gpio91) AND WS (gpio92) on the amp
pads, ADSP stays alive. During the ADSP's frozen START it sets the Quinary clock but never
writes any I2S_CTL slot (test #26). So the interface hardware is good; only the ADSP's start
path is broken. Full design + the two implementation approaches (A: own Linux LPAIF/DMA path;
B: redirect an ADSP Quaternary stream's DMA interface-select to Quinary) in
**`QUINARY-DRIVER-DESIGN.md`**. Next data needed (OPEN): decode the DMA CTL interface-select
field (Primary live CTL = 0x0000604f -> bits10-13 = 1) and pick a free read-DMA channel.

### Session 4 (2026-09-30): working-device comparison + daisy firmware plan
Real-world speaker status on mainline pmOS (user-confirmed):
- **Mi A2 Lite (daisy, MAX98927): speaker WORKS** (since 6.4.7, needs its UCM).
- **Mi A1 (tissot, MAX98927): works** (pmaports MR 4268).
- **Fairphone 3 (fp3, aw8898): speaker works** (only earpiece/mic fail = SLIMbus).
- Redmi 5 Plus (vince, TAS2557): speaker NOT supported.
- Mi Max 2 (oxygen, TAS2560): hangs (this project).
Pattern: dumb amps (MAX98927/Maxim, aw8898/Awinic, TFA) work on Quinary; the two TI smart-amps
(TAS2557/TAS2560) don't. BUT: **vince ADSP fw has 0 SmartAmp strings yet still hung on oxygen**,
so "SmartAmp = hang" is NOT the rule. vince is a bad reference (its Quinary never worked anywhere).
**daisy is the gold reference: its Quinary demonstrably starts.**
Decisive experiment (PENDING user OK to boot): run **daisy's ADSP firmware on oxygen** + our
tas2560.ko. daisy adsp extracted to `donor-fw/daisy/adsp/` (ADSP.VT.3.0-00100, 15 segs, no
SmartAmp, HW_ID 000460E1 = same SoC, same Xiaomi Root CA chain as oxygen -> should authenticate).
- If Quinary START succeeds -> hang is firmware-specific; daisy fw pumps I2S while tas2560.ko
  drives the amp over I2C -> path to real sound.
- If it also hangs -> the cause is oxygen board/boot-state, and donor-firmware is a dead end.
Also for the future: `ANDROID-APR-CAPTURE.md` = how to capture the working APR sequence from Android.

### Test #27 RESULT: daisy firmware ALSO hangs -> oxygen BOARD-specific
daisy ADSP fw (proven-working Quinary on Mi A2 Lite, no SmartAmp) booted fine on oxygen
(VT.3.0 authenticated) but Quinary START still gave -110, only BCLK on gpio91, no WS. So THREE
firmwares (oxygen/SmartAmp, vince/no-SmartAmp, daisy/no-SmartAmp-proven-working) all hang on
oxygen's Quinary. Same SoC/kernel/q6afe/config -> the hang is **oxygen board/boot-state
specific, NOT firmware**. Donor-firmware approach is CONCLUSIVELY ruled out.
Reconcile: quinws.ko proved Linux CAN drive oxygen's Quinary directly (produces WS). So the
hardware is fine; only the DSP's own Quinary-start path waits on an oxygen-board precondition
that never completes (FP3-class problem). => The **Linux bypass** (QUINARY-DRIVER-DESIGN.md) is
now the primary path; pivotal open question = can Linux drive the LPAIF read-DMA on this chip.

### Test #28 RESULT: Quaternary cannot reach the amp pads -> dead end
Quaternary playback on oxygen (works via DSP, amp PWR=0x40) drove ZERO tlmm pads (full scan
4..134, baseline + after mic_iomux QUA mux + after 0x0c055000 pri-mode-muxsel=1). Quaternary
is internally routed on oxygen and cannot be muxed to the pri_mi2s pads (the amp's pins). So
"piggyback working Quaternary -> amp pads" is ruled out.
Amp is on pri_mi2s pads (gpio88/91/92/93); only PRIMARY or QUINARY can drive that pad group.
Primary = internal codec (PDM, doesn't cleanly reach amp data). Quinary = hangs via DSP but
WORKS via direct Linux drive (quinws). => The Linux Quinary DMA bypass is the ONLY remaining
path. It is real driver work (port lpass-cpu/lpass-platform DMA to msm8953 Quinary, coexist
with DSP), not a one-shot test. Alternatively: Android APR capture (ANDROID-APR-CAPTURE.md)
could reveal the board precondition for a proper q6afe fix, when Android boot is possible.

### Session 4: DMA FEASIBILITY PROVEN (quindma/quindma.ko, normal OS, no reboot)
Driving an idle LPAIF read-DMA channel (ch1, 0x0c0d3000) from Linux while a tone plays on the
earpiece (shared LLB buffer at 0x0c0e0020): the DMA CURR register (+0xc) **ADVANCES** for
several interface-select values (bits10-13 = 0,2,3,4,6,7). => **Linux CAN drive the LPAIF DMA
engine on oxygen** (TZ does not lock it here). The Qualcomm "can't bypass" rule does not apply.
DMA reg layout confirmed (mainline lpass-platform + live): CTL+0 (enable b0, fifowm b1-5,
intf b10-13, burst b15-16), BASE+4 (LLB phys), BUFF+8 = words-1, CURR+0xc, PER+0x10 = words-1.
Remaining (ordinary bring-up, not blockers): (1) CURR advances ~1 FIFO depth then stalls =
interface not draining the DMA yet; need the right DMA<->interface connection. (2) amp data is
on Quinary SD2/SD3 = gpio94/95, NOT SD0/gpio88/93 (I sampled wrong pads); set I2S_CTL5 SPKMODE
for SD2/SD3 and sample gpio94/95. => The Linux Quinary bypass is FEASIBLE; next = wire DMA->slot5
so the interface drains it and clocks data on gpio94/95.

### Correction + next task: DMA<->interface routing
The DMA CTL bits10-13 are the WORDS-PER-SAMPLE count (ADSP config f0536040: r3|=asl(count,#0xa)),
NOT interface-select. So on msm8953 the DMA->interface connection is NOT in the DMA CTL (differs
from the documented apq8016 AUDINTF-in-CTL). It lives in a separate block near 0x0c0d0000
(ADSP getters read 0x0c0d0004/0x0c0d0008; base global 0xf0ce9c78=0x0c0cf000, +0x1004/+0x1008).
NEXT TASK to finish the bypass: RE the ADSP's DMA<->interface routing setter (search writes via
helper f05476b4 to the 0x0c0d0000 region) to learn how a read-DMA channel is bound to the
Quinary interface (slot5). Then: bind ch1->slot5, set I2S_CTL5 SPKMODE for SD2/SD3, sample
gpio94/95. FEASIBILITY IS PROVEN (DMA CURR advances from Linux); this routing decode + bring-up
is the remaining engineering.
