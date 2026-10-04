# Xiaomi Mi Max 2 (`oxygen`) loudspeaker on mainline Linux: research notes

**Status: not solved.** This folder collects everything learned while trying to
get the Mi Max 2's loudspeaker working on mainline postmarketOS (about 30 test
boots, a lot of register work and some ADSP firmware reverse engineering), so that
anyone can continue from here instead of starting over. The camera, LED torch and
SIM fixes elsewhere in this repository are independent of this.

What exists today: an ALSA UCM for the internal WCD codec (headphones,
microphones, see `ucm/`). What does not work: the **bottom loudspeaker**, and the
earpiece, which sits on the second TAS2560 on the same Quinary interface. The cause
is narrowed down a lot, but not found.

> If you only read one section, read [The problem](#the-problem),
> [Proven facts](#proven-facts) and [Where to go next](#where-to-go-next).

## Contents

1. [Hardware](#hardware)
2. [The problem](#the-problem)
3. [Proven facts](#proven-facts)
4. [Ruled out](#ruled-out)
5. [Open hypotheses](#open-hypotheses)
6. [Common suggestions](#common-suggestions-and-why-they-are-not-the-cause-here)
7. [Where to go next](#where-to-go-next)
8. [Register knowledge (LPASS on msm8953)](#register-knowledge-lpass-on-msm8953)
9. [Reverse-engineering results](#reverse-engineering-results)
10. [Android userspace facts](#android-userspace-facts)
11. [Mainline issues found along the way](#mainline-issues-found-along-the-way)
12. [Comparison with other msm8953 devices](#comparison-with-other-msm8953-devices)
13. [How to test safely](#how-to-test-safely)
14. [What is in this folder](#what-is-in-this-folder)
15. [Test index](#test-index)
16. [Glossary](#glossary)
17. [References](#references)
18. [License and credits](#license-and-credits)

## Hardware

| | Earpiece amp ("RCV", left) | Loudspeaker amp ("SPK", right) |
|---|---|---|
| Chip | TI **TAS2560** (smart amp, no on-chip DSP) | TI **TAS2560** |
| I²C | bus `i2c@78b6000` (`&i2c_2`, i2c-0), addr **0x4f** | same bus, addr **0x4c** |
| Reset / IRQ GPIO | 127 / 63 | 20 / 21 |
| Load | 8 Ω | 8 Ω |

- Both amps take audio over **Quinary MI2S** (interface slot 5 in LPASS). Downstream
  (`msm-ext-pa = "quinary"`): **RX on SD2+SD3** (`rx-lines 0x0c`), **IV-sense TX on
  SD0+SD1** (`tx-lines 0x03`).
- On msm8953, Quinary MI2S is muxed onto the **Primary MI2S pins**: gpio91 = BCLK,
  gpio92 = WS (`pri_mi2s_ws`), gpio88/93/94/95 = data (SD0..SD3). The
  `quin-iomux` register (`0x0c052000`) bit0 routes Quinary to those pins.
- Speaker ID: gpio9 is three-state (pull-down = SSI module, floating = AAC). It
  selects the SmartAmp tuning file `tas2560_ssi.bin` / `tas2560_aac.bin`.
- Android plays the speaker through **both** amps as stereo (mixer path `speaker` =
  `SPK Mixer SPK` + `RCV Mixer RCV`), 24-bit, with the SmartAmp feedback TX at 4 channels.
- The phone runs a **Qualcomm ADSP** (Hexagon) that owns LPASS audio. Mainline
  Linux talks to it through APR (`q6afe`, `q6asm`, `q6adm`, `q6routing`) like on every
  other msm8953 phone.

## The problem

Starting the Quinary AFE port through the ADSP **hangs the ADSP**:

```
q6afe-dai ...: AFE enable for port 0x1016 failed -110
```

`AFE_PORT_CMD_DEVICE_START` for port 0x1016 (Quinary MI2S RX) never answers, and the
whole ADSP audio service is dead until the next reboot (AFE and ADM time out).
Quinary TX (0x1017) hangs the same way. Primary, Secondary and Quaternary MI2S
start fine. During the hang the ADSP has already switched on the Quinary **bit
clock** (BCLK toggles on gpio91), but it **never writes any I2S interface
register**, so there is no word clock (WS) and no data (tests #14, #16, #26).

The same kernel (pmOS `linux-postmarketos-qcom-msm8953` 7.0.9), the same q6afe
code and the same Quinary configuration **work** on other msm8953 phones, for
example the Mi A2 Lite (`daisy`, MAX98927 amps) and the Fairphone 3 (aw8898).

## Proven facts

Each fact below was measured on the phone. The log for each test number is in
`evidence/logs/` (see the [test index](#test-index)).

| # | Fact | How we know |
|---|---|---|
| 1 | The ADSP hangs in Quinary `DEVICE_START`, before writing any I2S_CTL slot | pad sampling (#14, #16) and LPAIF register reads during the hang (#26) |
| 2 | **The Quinary hardware works.** Linux can drive the Quinary clock, the iomux and `I2S_CTL[5]` directly, and BCLK + WS then appear on gpio91/92 while the ADSP stays alive | `drivers/quinws/` (normal OS, no reboot; also #26) |
| 3 | It is **not the firmware**: three different ADSP firmwares hang the same way on oxygen: oxygen's own (has SmartAmp), vince's (no SmartAmp), daisy's (no SmartAmp, Quinary proven working on the Mi A2 Lite) | #5–#8, #27 |
| 4 | It is **not the Linux audio config**: sd-lines, TX/RX combos, v1/v2 bit clocks, AFE topology, OSR clock, pin drive strength, mux registers set to the exact downstream Android state: all still hang | see [Ruled out](#ruled-out) |
| 5 | Quaternary MI2S starts and plays, but it is routed internally and **cannot reach the amp pins** | #12, #13, #28 |
| 6 | Primary MI2S (internal WCD codec path) with the amp on the Primary link runs without DSP errors, puts WS on gpio92 and powers the amp (PWR=0x40), but gives **no sound** | #20, fallback test in `history/AUDIO-HANDOFF.md` |
| 7 | Linux can program an LPAIF read-DMA channel (TrustZone does not lock LPASS on this unlocked phone), **but the DMA only fills one FIFO (8 words) and then stalls**: the interface never drains it, for any interface-select value | `drivers/quindma/`, `drivers/quinroute/` (normal OS) |
| 8 | The DMA→interface selector is **DMA CTL bits 6-8** (not bits 10-13, which are words-per-sample): internal codec = 1, Quaternary = 4, i.e. probably `I2S slot + 1` | live register snapshots (#29, `evidence/regsnap/rt-*.bin`) |
| 9 | The ADSP never asks for any file (FastRPC `apps_std` / RFS over tqftpserv) during the hang, so it freezes **before** loading the SmartAmp tuning bin | hexagonrpcd under strace, tqftpserv logs |

## Ruled out

Each of these was tested on the phone, usually with one test boot each.

| Idea | Result |
|---|---|
| Wrong SD lines (`<0>`, `<2>`, `<2 3>`) | hangs |
| Quinary TX only (0x1017) | hangs |
| Bit clock: IBIT v2 (id 0x10B, same as downstream) or v1 `LPAIF_BIT_CLK` | hangs (`patches/0002`) |
| Explicit no-processing AFE topology (0x112FC/0x112FB, what the ACDB uses) | accepted, still hangs (`patches/0001`) |
| OSR clock `LPASS_CLK_ID_QUI_MI2S_OSR` before START | rejected (-22), hangs (#17) |
| Primary or Tertiary links present / removed; Primary stream running first | hangs (#18, #21, #11) |
| Pin config: 2 mA vs 8 mA, pull-down fixed | hangs (#9) |
| LPASS mux registers set to the exact downstream values (spkr 0x00010000, mic 0x00200000, quin 0x1) | hangs (#15, #25) |
| **Donor ADSP firmware** (vince, daisy) booted via DT `firmware-name` | both boot, both hang (#5–#8, #27) |
| **Fairphone 3 fix** (QDSP6SS register `0x0c20002c` bit 3, the PIL-vs-PAS difference) | cleared and even held clear during START: still hangs (#10, #19) |
| Missing codec MCLK | `INTERNAL_DIGITAL_CODEC_CORE` is already on (9.6 MHz) |
| Piggy-backing on the working Quaternary port | Quaternary drives none of the pads 4–134 (#28) |
| "SmartAmp in oxygen's firmware causes it" | vince/daisy firmware has no SmartAmp and hangs too, so not (only) that |

## Open hypotheses

1. **An oxygen-specific precondition the ADSP waits for** during the Quinary port
   start (a clock or power vote, a TZ/devcfg setting, or state that Android's
   boot chain or audio HAL sets up and mainline does not). This is the class of
   problem the Fairphone 3/5 had, and an Android capture solved it there.
2. **An unbounded wait inside the ADSP.** The ADSP power manager's clock enable
   (`halHwIo_EnableCgcClock` at `0xf019c26c`) has a status-poll loop without timeout
   at `0xf019c32c`. If Quinary needs a clock branch that never reports "on", the ADSP
   would freeze exactly like this. Not tied to Quinary yet.
3. For the Linux bypass: **a missing DMA↔interface handshake**. With DMA, selector,
   `I2S_CTL[5]` and the bit clock all set from Linux, the interface still does not
   consume the DMA. Something only the ADSP's port start sets is still missing
   (candidates: the routing registers `0x0c0d0008` / `0x0c0d1000`, see below).

## Common suggestions (and why they are not the cause here)

**"The ADSP firmware isn't loaded, so the driver talks to nothing."**
It is loaded. postmarketOS's `msm-firmware-loader` provides Xiaomi's own ADSP
firmware (`adsp.mdt` + `adsp.b00`…`b13` from the modem partition) and remoteproc
boots it: the test logs show `remote processor adsp is now up` in every boot. The
only `Direct firmware load … failed` message in all logs is for
`WCNSS_qcom_wlan_nv.bin` (Wi-Fi calibration, unrelated). A running ADSP is also
proven by what works through it: Primary, Secondary and Quaternary MI2S ports start,
Quaternary plays and powers the amp (#12), the APR services answer, FastRPC and
the ADSP's own file access (tqftpserv) work. Only the Quinary port start hangs.
Two other firmwares were booted too (#5–#8, #27), with the same result.

**"Android's `mixer_paths.xml` is tinyalsa/HAL syntax and can't be mapped to ALSA."**
Correct, and it wasn't mapped. `mixer_paths_mtp.xml` was only read for facts (which
amps the speaker uses, bit width, DAC volume). The mainline routing uses the ALSA
controls of `q6routing` (`QUIN_MI2S_RX Audio Mixer MultiMedia3`) and of the TAS2560
driver. And the failure is below ALSA routing: the kernel's own
`AFE_PORT_CMD_DEVICE_START` to the ADSP times out (`AFE enable for port 0x1016
failed -110`), and the pads show the ADSP never enables the interface. A wrong
mixer route gives silence or "no backend DAIs", not a dead DSP.

**The part of that idea that does matter:** Android's audio HAL also sends
calibration data (ACDB) and extra parameters to the ADSP through the kernel, which
mainline never sends. The ACDB was decoded (every device uses the no-processing AFE
topology, which was also sent explicitly, see [Ruled out](#ruled-out)), but other
calibration blocks can't be excluded from here. That is exactly what the
[Android capture](#a-android-capture-recommended-decisive) records: every packet the
working Android stack sends, including calibration.

## Where to go next

### A. Android capture (recommended, decisive)

Record the exact APR commands that **working Android** sends to the ADSP when the
loudspeaker plays, then diff them against what mainline `q6afe` sends. This is how
the Fairphone 3/5 speaker was fixed
([pmaports#3793](https://gitlab.postmarketos.org/postmarketOS/pmaports/-/issues/3793)).

- `android-capture-kit/` has a self-checking script (`capture-speaker-dsp.sh`) plus
  plain-language instructions (`CAPTURE-README.txt`). It needs a **rooted** Android
  phone whose loudspeaker works. The best source is a Mi Max 2 on Android (same
  firmware and board). Any msm8953 phone is still useful.
- It kprobes `apr_send_pkt`, captures the speaker playback plus mixer/ACDB/firmware
  context, and writes one text file. `history/ANDROID-APR-CAPTURE.md` explains the
  manual method and what to look for (opcodes `0x100e5` DEVICE_START, `0x100ef` /
  `0x100f3` SET_PARAM around port 0x1016).

**If you have a rooted msm8953 Android phone, running this kit is the single most
useful contribution.**

### B. Linux-driven Quinary (DSP bypass)

Let the ADSP keep everything else and drive only the Quinary interface and one
LPAIF read-DMA channel from Linux. The design, the register map and every result
are in `history/QUINARY-DRIVER-DESIGN.md`. The state:

- Proven: the interface runs from Linux (BCLK + WS), and the DMA engine accepts
  Linux programming.
- **Blocker:** the interface does not drain the DMA (fact 7). The next step is to
  reverse-engineer how the ADSP establishes a draining DMA→interface connection:
  the setter for **`0x0c0d0008`** (live `0x08008001` during earpiece playback,
  `0x1` during Quaternary; firmware field hint: bits 8-11, bit 16, bits 24-31) and
  the enable in **`0x0c0d1000`** (`0x00038007` / `0x7`). Avoid blind sweeps of these
  registers: reading the whole LPAIF window once killed the ADSP.
- If that works: turn `quinroute` into a small ASoC platform/CPU driver (modelled
  on `sound/soc/qcom/lpass-cpu.c` + `lpass-platform.c`) that owns only slot 5,
  put `tas2560` on its DAI link, and add a UCM "Speaker" device.

### C. Also worth trying

- Ask the msm8953-mainline people (for example on the Fairphone 3 issue
  [msm8953-mainline#255](https://github.com/msm8953-mainline/linux/issues/255))
  whether anyone has seen a Quinary START that hangs only on one board.
- The Primary-link path (fact 6) ran without DSP errors and without sound. Revisit
  it with **both** amps and different TAS2560 ASI settings (format, slot, data delay),
  since it avoids the Quinary start completely. The amp driver's register access is
  already in place.

## Register knowledge (LPASS on msm8953)

LPASS base `0x0c000000`. From the ADSP firmware's devcfg/clock tables, confirmed by
live reads where noted.

| What | Address / value |
|---|---|
| Quinary bit clock RCG | `0x0c032000` (CMD +0, CFG +4, M +8, N +0xc, D +0x10). 1.536 MHz = CFG `0x2513`, M 1, N `0xf8`, D `0xf7`. UPDATE = CMD bit0 |
| Quinary clock branches | OSR `0x0c032014`, IBIT `0x0c032018` (CBCR bit0 = enable, bit31 = CLK_OFF), EBIT `0x0c03201c` |
| Other interface branches | pri `0x0c00b014..1c`, ter `0x0c00d0xx`, quat `0x0c00e0xx`, senary `0x0c033xxx`, digcodec `0x0c02c014`, mclk0/1 `0x0c034014` / `0x0c035014` |
| I2S interface control | `I2S_CTL[n] = 0x0c0c4000 + n*0x1000`, n = 0..6, reset `0x000f0004`. **Quinary = slot 5 (`0x0c0c9000`)**. Working value `0x000f4400` (SPKEN bit14, SPKMODE bits 10-13, WS internal bit2, 16-bit). SPKMODE for SD2/SD3 (QUAD23) = `0x6 << 10` |
| Read-DMA channels | `0x0c0d2000 + ch*0x1000`: CTL +0 (enable b0, FIFO watermark b1-5, **interface select b6-8**, words-per-sample b10-13, burst b15-16), BASE +4, BUFF_LEN +8 (words-1), CURR +0xc, PER_LEN +0x10 |
| DMA routing block | base `0x0c0cf000`: `0x0c0d0004`, `0x0c0d0008`, `0x0c0d000c`, `0x0c0d1000` (see [Where to go next](#b-linux-driven-quinary-dsp-bypass)) |
| Mux CSRs | `0x0c050000` block: mic iomux, spkr iomux (`PRI_WS_SLAVE_SEL` bits 16-17), **quin iomux `0x0c052000` bit0** |
| QDSP6SS (FP3 bit) | `0x0c20002c` reads `0x10b` under PAS (bit3 set), `0x103` under downstream PIL |

Live snapshots of these blocks (idle, Quaternary playing, Quinary hung) are in
`evidence/regsnap/`. `drivers/regsnap/diff.py` compares two snapshots.

## Reverse-engineering results

Everything in this section comes from reading the oxygen ADSP firmware
(`ADSP.8953.2.8.2`, Xiaomi build path
`/home/work/oxygen-n-stable-build/vendor/qcom/non-hlos-msm8953-spf20/ADSP.8953.2.8.2/`)
plus live register reads. Addresses starting with `0xf0…` are ADSP virtual
addresses in that firmware; `0x0c…` are physical LPASS addresses.

### Getting and reading the firmware

- The firmware is **not** included here. Take it from your own phone: the modem
  partition's `image/adsp.mdt` + `adsp.b00..b13` (on postmarketOS it is mounted at
  `/run/msm-firmware-loader/mnt/modem/image/`).
- The `.mdt` holds the ELF header and program headers, the `.bNN` files are the
  segments. Rebuild one ELF from them and disassemble with
  `llvm-objdump --triple=hexagon` (Alpine's llvm includes the Hexagon target;
  Ghidra with a Hexagon plugin also works).
- The firmware keeps its **debug messages**: each is a struct
  `{u16 line, u16 ssid (0x2134 = QDSP6), u32 mask, char *fmt}` in the table
  `0xf07b9f00…0xf07baf60`, so every log call maps to a code address and source line
  (`AFESmartamp.cpp`, `AFEPortAprHandler.cpp`, …). `adsp-re/annot.py` annotates a
  disassembly with these strings, `adsp-re/cg.py` walks the call graph.
- SmartAmp code is in segment `b07`; `rfsa_client`, `apps_std`, `fopen`,
  `ADSP_LIBRARY_PATH` and the `tas2560_*.bin` names are in `b04`.

### SmartAmp (TI speaker protection inside the ADSP)

- **The ADSP turns SmartAmp on by itself.** In `afe_port_apr_msg_handler`, opcode
  `0x100E5` (`AFE_PORT_CMD_DEVICE_START`) for port 0x1016 or 0x1017 calls
  `SmartAmp_enable` (`0xf0553e7c`) unconditionally, then the normal port start
  (`0xf054cf88`). Linux cannot avoid it by simply not sending a parameter.
- `SmartAmp_enable`: allocates its state (at port + `0x2f4`), makes an MMPM power
  vote (`0xf054b77c`, the generic AFE MMPM function), sets enabled = 1. A failed
  vote only logs "disabling the module" (not fatal).
- On port start, if enabled, `SmartAmp_init` (`0xf0553f38`): library-size query,
  **tuning file load** (`0xf0553674`: `fopen("tas2560_ssi.bin" / "tas2560_aac.bin", "a+b")`
  through the ADSP libc, i.e. FastRPC `apps_std`), buffer allocations, sets bits in
  the global mask `0xf0cf87cc` (bit2 = RX 0x1016, bit1 = TX 0x1017), and launches a
  dynamic thread `"AfeDy%2lx"` (`0xf0549a68`).
- The processing callbacks (`0xf0553318` TX, `0xf05533ec` RX) only run the algorithm
  when the mask is 6, i.e. **RX and TX both active**. RX needs 1–2 channels ("not
  stereo mode" otherwise), TX 2 or 4.
- Parameters (from Xiaomi's `smart_amp.h`): module `AFE_SMARTAMP_MODULE 0x0F010209`,
  `AFE_PARAM_ID_SMARTAMP_DEFAULT 0x10001166`; the algorithm-control param id is
  `index | (len << 16) | (slave << 24)`. The firmware only accepts enable/disable in
  the CONFIG or STOP state (`Smartamp en/disable=%d failed, Accept in CONFIG/STOP state only`).
  Also present: `AFE_SA_SET_SPKID`, `SPK_RE`, `RCV_RE`, and profiles
  MUSIC / VOICE_HANDSET / VOICE_HANDSFREE / RINGTONE / MOVIE-GAME / FCT.
- Xiaomi's Android kernel uses the SmartAmp module only for calibration get/set
  (`drivers/tas_calib`). Android sends nothing special before START.
- Qualcomm's own speaker-protection modules (`capi_v2_sp_v2_*`) are in the firmware too.
- Because no file request ever reached Linux during a hang, the freeze is **before**
  the tuning-file load. And because firmwares without SmartAmp hang too, SmartAmp is
  not (or not only) the cause.

### How the ADSP reads files

- The rcinit task `rfsa_client` (entry `0xf01c92a4`) sets up RFS over TFTP, served
  on Linux by `tqftpserv`. The ADSP writes `/readwrite/server_check.txt` on every
  boot, and nothing else was requested in any boot, including all hangs.
- `apps_std` file access goes through FastRPC, served on Linux by `hexagonrpcd`
  (mainline `drivers/misc/fastrpc.c`, which pmOS does not build; build it as a
  module). It works (the ADSP asks for `remote_heap_config.so` at attach), but under
  `strace` there was **no** request at Quinary start.

### LPASS map from the firmware

- Clock branches (LPASS base `0x0c000000`): pri `0x0b014/18/1c`, ter `0x0d0xx`,
  quat `0x0e0xx`, **Quinary `0x32014` (OSR) / `0x32018` (IBIT + CDIV) / `0x3201c` (EBIT)**,
  senary `0x33xxx`, digital codec `0x2c014`, mclk0/1 `0x34014` / `0x35014`.
- LPAIF block `0x0c0c0000` (+`0x20000`), 7 I2S slots, the devcfg interface list is
  `0, 1, 2, 3, 5, 6` (Quinary = slot 5). Mux CSR block `0x0c050000`.
- The LPAIF base is kept in the global `0xf0ce9c78` = `0x0c0cf000` (a second instance in
  `0xf0ce9c7c`; the word-offset global `0xf0ce9c80` is 0 on this build).

### I2S and DMA hardware layer

- I2S HAL (V1, function table `0xf0cf0728`): init `f0539868`, config `f05397c8`
  (I2S_CTL: SD mode bits 10-13, mono bit 9, WS source bit 2, bit width bits 0-1,
  write mask `0x8007 | 0x3e00`), **enable `f0539794` = set bit 14** for RX (`0x100` for
  TX), disable `f0539764`, index→IRQ map `f053971c`. All writes go through the
  read-modify-write helper `f05476b4` (`reg = (reg & ~mask) | val`). **No wait loops**
  anywhere in the I2S/DMA layer.
- DMA config `f0536040` writes the **words-per-sample** count into CTL bits 10-13
  (`r3 |= asl(count, #0xa)`). The CTL write mask is `0x1bc3e`.
- DMA routing getter `f053613c` reads `0x0c0d0008` and `0x0c0d0004`. The setter
  `f0535ef8` (and `f05362ac` for instance 1, which also ORs `0x1800000`) writes one of 4
  table patterns (table at `0xf07b6710…67ac`, indexed by interface index mod 4:
  `0x00008038`, `0x000101c0`, `0x00020e00`, 0) to `0x0c0d000c` / `0x0c0d0004`. These are
  three 3-bit fields (bits 3/6/9) with enables (bits 15/16/17). Writing them from Linux
  did **not** bind the DMA to Quinary, so playback routing lives in `0x0c0d0008`
  (field hint from descriptor function `f0182078`: bits 8-11, bit 16, bits 24-31).
- Live values: during earpiece playback `0x0c0d0008 = 0x08008001`,
  `0x0c0d1000 = 0x00038007`; during Quaternary playback `0x0c0d0008 = 0x1`,
  `0x0c0d1000 = 0x7`, read-DMA ch0 CTL = `0x610f` (interface select 4). With a hung
  Quinary the whole block reads `0x210f` (ADSP dead).

### Power manager

The ADSP power manager's clock enable `halHwIo_EnableCgcClock` (`f019c26c`) polls a
status bit **without a timeout** (loop at `f019c32c`) for clocks that have a wait
mask. A clock that never reports "on" would freeze the ADSP exactly the way the
Quinary start does. Not yet connected to Quinary; a good lead for anyone with
runtime visibility into the ADSP.

### ADSP debug output (DIAG)

`rpmsg_char` can be bound to `remoteproc*:smd-edge.DIAG` / `DIAG_CNTL` with
`driver_override` (the kernel has no `rpmsg_ctrl` for it). The control handshake
works (feature mask, command registrations, SSID ranges), but no F3 debug messages
were received yet (`scripts/adsp-diag.py`, experimental). **Warning** (from the
Fairphone 3 thread): binding `DIAG_CNTL` without doing the handshake can reset the SoC.

### Donor firmware and signatures

- vince (Redmi 5 Plus) ADSP from `fw_vince V11.0.2.0` NON-HLOS.bin, and daisy (Mi A2
  Lite) `ADSP.VT.3.0-00100` from `miui_DAISYGlobal_V11.0.21.0.QDLMIXM` (both as Xiaomi
  Flashable Firmware Creator packages from XiaomiFirmwareUpdater). Neither contains
  SmartAmp.
- Both are signed by the **same Xiaomi Root CA 1 chain** with the same HW/OEM/model
  IDs (HW_ID `000460E1` = msm8953), so they **authenticate and boot on oxygen**
  (checked with `adsp-re/certs.py`). Boot them through the DT `&lpass { firmware-name = …; }`,
  not by stopping and swapping the ADSP at runtime (that hung the phone).

## Android userspace facts

From the Android vendor partition (read it read-only, e.g. with
`dmsetup create --readonly` over the partition range and `mount -o ro,noload`; the
offsets are device-specific):

- `mixer_paths_mtp.xml`: `speaker` = `SPK Mixer SPK` + `RCV Mixer RCV` (stereo on both
  amps), `handset` = RCV only, `vi-feedback` path is empty,
  `TAS2560 Speaker/Receiver DAC Volume 14db`, speaker bit width 24.
- The speaker uses ACDB id 34, which the QRD ACDB calls `HANDSET_MIC_STEREO`, so
  Xiaomi's ACDB mapping is unusual.
- **ACDB file format**: `QCMSNDDB` header, then chunks (8-character tag, u32 length,
  data). `DPROPLUT` = u32 n + n × `{device, property id, data-pool offset}`; `DATAPOOL`
  entries = u32 length + data. Property `0x113b8` = device name (UTF-16),
  `0x13150` = AFE topology. Every device uses the no-processing AFE topology
  (RX `0x112FC`, TX `0x112FB`).
- No vendor program uses `tas2560` / `tas_calib`. `adsp_avs_config.acdb` only
  registers Dirac and aptX, and `/dsp` only holds decoders/post-processing.
- The LineageOS 4.9 kernel does the same as Xiaomi's 3.18 one for this path.

## Mainline issues found along the way

Worth reporting upstream regardless of the speaker:

1. **oxygen DT pin states merge.** In the oxygen device tree, `tlmm_pri_act` and
   `tlmm_pri_sus` are both named `pri-tlmm-state`, so the two nodes merge and the
   active Primary MI2S pin state ends up as 2 mA with pull-down. Fix: give them
   distinct node names (e.g. `pri-tlmm-active-state` / `pri-tlmm-sleep-state`).
2. **Stale DPCM route in `q6routing`.** A mixer control like
   `QUIN_MI2S_RX Audio Mixer MultiMedia3` can read "on" while the DAPM path is not
   connected (`no backend DAIs enabled for MultiMedia3`). The `put()` handler returns
   early when `session->port_id == be_id` (`q6routing.c`, around line 500), which looks
   stale after the sound card is re-created. Workaround in our scripts: toggle the
   control 0 → 1 and check `MM_DL3 → QUIN_MI2S_RX Audio Mixer` in debugfs before playing.
3. `CONFIG_QCOM_FASTRPC` is not enabled in the pmOS msm8953 kernel config, so
   `hexagonrpcd` cannot serve the ADSP's file requests without building it yourself.

## Comparison with other msm8953 devices

Status as known when this was written. "In-tree" refers to the msm8953-mainline
7.0.9 device trees used here.

| Device | Speaker amp | Loudspeaker on mainline | Notes |
|---|---|---|---|
| Xiaomi Mi A2 Lite (`daisy`) | Maxim MAX98927 (in-tree) | ✅ works (since 6.4.7, needs its UCM) | **best reference**: same SoC, same Quinary on the Primary pins; its ADSP firmware still hangs on oxygen (#27) |
| Xiaomi Mi A1 (`tissot`) | Maxim MAX98927 (in-tree) | ✅ works | [pmaports!4268](https://gitlab.postmarketos.org/postmarketOS/pmaports/-/merge_requests/4268) |
| Fairphone 3 (`fp3`, SDM632) | Awinic aw8898 | ✅ works (earpiece/mics fail: SLIMbus) | QDSP6SS bit-3 fix is SLIMbus-only ([#255](https://github.com/msm8953-mainline/linux/issues/255)); tested here, not our fix |
| Asus Zenfone 3 (ZE520KL / ZE552KL) | NXP TFA9895 (per our notes) | unverified | the in-tree DTs have no speaker amp or Quinary node |
| Motorola G7 Power (`ocean`, SDM632) | NXP TFA9874 | in-tree DT with Quinary | |
| Xiaomi Redmi 5 Plus (`vince`) | TI TAS2557 | ❌ not supported | no amp in the in-tree DT; its Quinary never worked anywhere, so it is a weak reference ([kotXio/postmarketos-xiaomi-vince](https://github.com/kotXio/postmarketos-xiaomi-vince)) |
| Lenovo CD-18781Y | | | not in this kernel tree; per our notes its Quaternary goes to the sec_mi2s pins (135-138), consistent with oxygen's Quaternary reaching no amp pin (#28) |
| **Xiaomi Mi Max 2 (`oxygen`)** | **TI TAS2560 ×2** | ❌ Quinary START hangs | this project |

Pattern: the phones known to work (daisy, tissot, Fairphone 3) use simple
amplifiers on Quinary; the two with TI smart amps (TAS2557, TAS2560) do not. But the firmware swap tests show the hang is not
caused by the firmware's SmartAmp alone, and no board difference visible from Linux
(audio DT, LPASS clocks, regulators, pins, reserved memory) explains it.

## How to test safely

Read this before touching a phone.

- **Never flash for experiments.** Build a RAM-boot image (the phone's own
  `/boot/vmlinuz` + a test DTB + `/boot/initramfs`) and `fastboot boot` it
  (`scripts/mkramboot.sh`). Our test phone's eMMC is failing, so writing partitions
  was avoided entirely.
- **One Quinary attempt per boot.** A hang wedges the ADSP until reboot, so put
  all other checks before it.
- **Always return to the normal OS afterwards** and check that `/proc/cmdline`
  starts with `quiet splash` and `/dev/dri/card0` exists. A reboot from a test image
  can come back into the test image, which leaves a black screen.
  `scripts/testboot.sh` does this (needs SSH key login to the phone, `ssh-copy-id user@172.16.42.1`, and `PHONE_PASS` set to the phone's password for sudo).
- Keep the volume low (`-20 dBFS` tone, amp gain 0–6 dB). Android's SmartAmp
  normally protects these speakers and mainline has no such protection.
- **Never read the whole LPAIF window** while the ADSP runs: it killed the ADSP.
  Read single known registers.
- **Don't stop the ADSP while donor firmware runs**: it hung the phone. Boot donor
  firmware through the DT `firmware-name` instead.
- Building modules: every module here is out-of-tree, e.g.
  `make ARCH=arm64 LLVM=1 O=<full build of the running kernel> M=drivers/quinws modules`.
  The kernel must match the phone exactly (pmOS 7.0.9-msm8953 has no MODVERSIONS or
  module signing, so a full build with the pmaports config works; see the camera
  part's `kernel/build-modules.sh` for a complete recipe).
- Replacing an in-tree module in a test boot: add `module_blacklist=q6afe` (or
  `modprobe.blacklist=snd_soc_apq8016_sbc`) to the cmdline, then `insmod` the patched
  copy and its dependents by path (`scripts/topo-test.sh`).
- Useful routes: Quinary `amixer -c0 cset "name=QUIN_MI2S_RX Audio Mixer MultiMedia3" 1`
  + `aplay -D hw:0,2`. Amp status while playing: `i2ctransfer -f -y 0 w2@0x4c 0x00 0x00;
  i2ctransfer -f -y 0 w1@0x4c 7 r1` (0x40 = on, 0x01 = shutdown).

## What is in this folder

| Path | What |
|---|---|
| `drivers/tas2560/` | **TAS2560 ASoC codec driver** for mainline (paged regmap, reset, PLL from BCLK, Xiaomi's power-up sequence, IV-sense capture). All 172 coefficient bytes verified against Xiaomi's driver. DT: `ti,load-ohms`, `ti,asi-channel`. Works as far as it can be tested (the amp powers up and reports no errors) |
| `drivers/quinws/` | drives the Quinary clock + iomux + `I2S_CTL[5]` from Linux (fact 2) |
| `drivers/quindma/`, `drivers/quinroute/` | Linux LPAIF read-DMA + interface bring-up attempts (fact 7). Parameters: `ch`, `spkmode`, `i2sval`, `slot`, `dry` |
| `drivers/padsample/` | samples TLMM pad input 200k times to see which pins toggle (`first=`, `samples=`, `tag=`) |
| `drivers/regsnap/`, `drivers/regdump/` | snapshot an MMIO range to debugfs / print the LPASS mux registers |
| `drivers/lpassmux/` | set/clear bits in the LPASS mux registers |
| `drivers/qdsp6ss-bit/` | the Fairphone 3 QDSP6SS bit test (**dead end**, kept for completeness) |
| `patches/0001-*` | `q6afe` test knobs: `quin_none_topology`, `quin_osr_hz`, `pri_sd_mask`, `quat_sd_mask` |
| `patches/0002-*` | `apq8016_sbc` test knob `quin_legacy_clk` (v1 LPAIF bit clock for Quinary) |
| `dts/` | every test device tree (`-audio*`, `-quat*`, `-quin*`). `-audio.dts` builds on `msm8953-xiaomi-oxygen-imx386.dts` (included) |
| `scripts/` | test runners (`testboot.sh`, `*-test.sh`), `mkramboot.sh`, `adsp-diag.py` (experimental ADSP DIAG client), `make-beeps.py` (writes `beeps-1.wav`..`beeps-4.wav`: N beeps, so a listener can report which variant played). The on-phone scripts expect the built `.ko` files and the wav files in `/home/user` (postmarketOS's default user) |
| `ucm/` | ALSA UCM for oxygen (headphones and mics; no Speaker device yet) |
| `android-capture-kit/` | [path A](#a-android-capture-recommended-decisive) |
| `adsp-re/` | `annot.py` (annotate a disassembly with the firmware's debug strings), `cg.py` (call-graph walker), `certs.py` (list the signing certificates of a firmware image). Bring your own firmware |
| `evidence/logs/` | the log of every numbered test |
| `evidence/regsnap/` | register snapshots (LPASS clock controller, LPAIF, routing block) in the named states |
| `history/` | the original working notes (`AUDIO-HANDOFF.md` = full session log, `QUINARY-DRIVER-DESIGN.md`, `ANDROID-APR-CAPTURE.md`). They contain superseded ideas; this README is the corrected summary |

`fastrpc` was also built as a module during the work. It is the unmodified mainline
`drivers/misc/fastrpc.c` (`CONFIG_QCOM_FASTRPC` is off in pmOS), so it is not included.

## Test index

| # | Log (`evidence/logs/`) | What | Result |
|---|---|---|---|
| 1–4 | `vince-quin-test*.log` | runtime swap to vince ADSP firmware | inconclusive (swap problems), see history |
| 5–8 | `vince-boot-test5..8.log` | vince firmware via DT, oxygen and vince-style Quinary config | hangs |
| 9 | `quin-min-test9.log` | minimal DT, vince-style Quinary, 8 mA pins | route never connected; phone hung on restore |
| 10 | `bit3-test10.log` | clear QDSP6SS bit 3 (FP3 fix) before START | hangs |
| 11 | `mm1-test11.log` | vince UCM route MM1 → PRI + QUIN | hangs, then Primary dies too |
| 12 | `quat-test12.log` | Quaternary START, all SD lines | **works**, amp PWR 0x40, no sound |
| 13 | `quat-pads-test13.log` | pads during Quaternary | no pri_mi2s pad toggles |
| 14 | `quin-pads-test14.log` | pads during the Quinary hang | only gpio91 (BCLK) toggles |
| 15 | `quin-wsmux-test15.log` | clear `PRI_WS_SLAVE_SEL` | hangs |
| 16 | `quin-scan-test16.log` | scan all pads 4–134 during the hang | only BCLK anywhere |
| 17 | `quin-osr-test17.log` | request the Quinary OSR clock | rejected, hangs |
| 18 | `quin-fp3style-test18.log` | Fairphone-3-style card (Quinary only) | hangs |
| 19 | `quin-hold-test19.log` | hold QDSP6SS bit 3 clear during START | hangs |
| 20 | `pri-pads-test20.log` | Primary path pads | WS on gpio92, amp on, no sound |
| 21 | `quin-after-pri-test21.log` | Primary stream first, then Quinary | hangs |
| 22–24 | `snap-quat-test22.log`, `snap-quin-test23.log`, `lpaif-snap-test24.log` | register snapshots | see `evidence/regsnap/`. A full-window read killed the ADSP |
| 25 | `quin-dsmux-test25.log` | mux registers = downstream values | hangs |
| 26 | `quin-slots-test26.log` | read all I2S_CTL slots during the hang | ADSP never writes any slot |
| 27 | `quin-daisy-test27.log` | daisy firmware (proven-working Quinary) | hangs |
| 28 | `quat-route-test28.log` | can Quaternary reach the amp pads? | no |
| 29 | `quat-routing-test29.log` | routing block during idle / Quaternary / Quinary | DMA selector found (fact 8) |

## Glossary

- **ADSP**: the Hexagon DSP in the SoC that runs Qualcomm's audio firmware.
- **APR**: the message protocol Linux uses to talk to the ADSP's audio services.
- **AFE**: the ADSP's audio front end service; one "port" per audio interface (Quinary RX = 0x1016).
- **LPASS**: the low-power audio subsystem (clocks, I2S interfaces, DMA) that the ADSP drives.
- **LPAIF**: the LPASS audio interface block (I2S controllers + DMA).
- **MI2S**: Qualcomm's multi-channel I2S. msm8953 has Primary, Secondary, Tertiary, Quaternary, Quinary.
- **BCLK / WS / SD**: bit clock, word select (frame clock), serial data lines.
- **PIL / PAS**: two ways to boot a remote processor: downstream kernel loader vs TrustZone "peripheral authentication service" (mainline).
- **SmartAmp**: Xiaomi/TI speaker-protection algorithm that runs inside the ADSP for the TAS2560.

## References

**This phone**
- postmarketOS wiki: [Xiaomi Mi Max 2 (xiaomi-oxygen)](https://wiki.postmarketos.org/wiki/Xiaomi_Mi_Max_2_(xiaomi-oxygen))
- Xiaomi's oxygen kernel source (3.18), the main reference for the TAS2560 driver and the downstream audio path: [MiCode/Xiaomi_Kernel_OpenSource `oxygen-n-oss`](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/oxygen-n-oss). Files used: `sound/soc/codecs/tas2560*`, `sound/soc/msm/msm8952*.c`, `sound/soc/msm/qdsp6v2/q6afe.c`, `sound/soc/msm/qdsp6v2/msm-dai-q6-v2.c`, `include/sound/apr_audio-v2.h`, `include/sound/q6afe-v2.h`, `include/sound/smart_amp.h`, `drivers/tas_calib/`
- LineageOS oxygen kernel (4.9), cross-check of the same path: [Deeping415/android_kernel_xiaomi_oxygen `lineage-23.2`](https://github.com/Deeping415/android_kernel_xiaomi_oxygen/tree/lineage-23.2). Files used: `oxygen/audio.dtsi`, pinctrl, `techpack/audio/asoc/msm8952.c`, `techpack/audio/asoc/msm-dai-q6-v2.c`, `techpack/audio/dsp/q6afe.c`

**Kernel and packages used**
- Kernel: [msm8953-mainline/linux](https://github.com/msm8953-mainline/linux), tag `v7.0.9-r0`, as packaged by pmaports: [linux-postmarketos-qcom-msm8953](https://gitlab.postmarketos.org/postmarketOS/pmaports/-/tree/v26.06/device/community/linux-postmarketos-qcom-msm8953)
- Mainline sources the work is based on: [`q6afe.c`](https://github.com/torvalds/linux/blob/master/sound/soc/qcom/qdsp6/q6afe.c), [`q6routing.c`](https://github.com/torvalds/linux/blob/master/sound/soc/qcom/qdsp6/q6routing.c), [`apq8016_sbc.c`](https://github.com/torvalds/linux/blob/master/sound/soc/qcom/apq8016_sbc.c), and for the Linux-bypass design the LPASS drivers [`lpass-cpu.c`](https://github.com/torvalds/linux/blob/master/sound/soc/qcom/lpass-cpu.c), [`lpass-platform.c`](https://github.com/torvalds/linux/blob/master/sound/soc/qcom/lpass-platform.c), [`lpass-apq8016.c`](https://github.com/torvalds/linux/blob/master/sound/soc/qcom/lpass-apq8016.c), [`lpass-lpaif-reg.h`](https://github.com/torvalds/linux/blob/master/sound/soc/qcom/lpass-lpaif-reg.h); FastRPC [`fastrpc.c`](https://github.com/torvalds/linux/blob/master/drivers/misc/fastrpc.c)
- ADSP helpers on Linux: [hexagonrpc (hexagonrpcd)](https://github.com/linux-msm/hexagonrpc), [tqftpserv](https://github.com/linux-msm/tqftpserv), [msm-firmware-loader](https://gitlab.postmarketos.org/postmarketOS/msm-firmware-loader)

**Other devices**
- Fairphone 3 audio on msm8953-mainline: [msm8953-mainline/linux#255](https://github.com/msm8953-mainline/linux/issues/255)
- The Android APR capture that fixed Fairphone 3/5: [pmaports#3793](https://gitlab.postmarketos.org/postmarketOS/pmaports/-/issues/3793)
- Mi A1 (tissot) speaker: [pmaports!4268](https://gitlab.postmarketos.org/postmarketOS/pmaports/-/merge_requests/4268)
- Redmi 5 Plus (vince) postmarketOS work (TAS2557, same kernel version): [kotXio/postmarketos-xiaomi-vince](https://github.com/kotXio/postmarketos-xiaomi-vince)
- Mainline TAS2557 driver discussion: [linux-sound thread](https://ratatoskr.run/linux-sound/2026/07/17277306/t)

**Donor firmware**
- [XiaomiFirmwareUpdater](https://github.com/XiaomiFirmwareUpdater) packages (Xiaomi Flashable Firmware Creator): [vince firmware](https://xmfirmwareupdater.com/firmware/vince/) (`fw_vince V11.0.2.0`), [daisy firmware](https://xmfirmwareupdater.com/firmware/daisy/) (extracted from `miui_DAISYGlobal_V11.0.21.0.QDLMIXM`)

## License and credits

- GPL-2.0, like the main repository (see `../LICENSE`). The device trees in `dts/`
  are BSD-3-Clause, as stated in their headers. `drivers/tas2560/` is derived from
  Xiaomi's GPL TAS2560 driver in the oxygen kernel source.
- `evidence/` holds measurements from our test phone. No Qualcomm or Xiaomi
  firmware, libraries, disassembly or strings extracted from them are included.
- Thanks to Kostiantyn Andriiuk (postmarketos-xiaomi-vince), the msm8953-mainline
  and postmarketOS people, and everyone who worked out the Fairphone 3/5 audio.
