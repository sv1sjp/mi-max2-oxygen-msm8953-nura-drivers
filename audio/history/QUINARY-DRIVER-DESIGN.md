# Mi Max 2 speaker — Linux-driven Quinary MI2S (design)

Status: design only. Written 2026-09-29 from measured/decoded facts (see AUDIO-HANDOFF.md
"Session 3"). **OPEN** = still needs data.

## Why bypass the ADSP for Quinary
- The ADSP's Quinary START (AFE port 0x1016) freezes before it writes any LPAIF interface
  register — reproduced ~20 times, with oxygen's and vince's ADSP firmware and every config.
- The Quinary **hardware is fine**: `quinws.ko` (normal OS) set the Quinary bit clock, quin
  iomux and `I2S_CTL[slot 5] = 0x000f4400` directly, and BCLK (gpio91) + WS (gpio92) appeared
  on the amp pads; the ADSP stayed alive.
- Plan: leave the ADSP owning Primary/Tertiary/WCD/clocks; let Linux drive only the Quinary
  interface (slot 5) and one LPAIF read-DMA channel.

## Known register blocks (LPASS, phys base 0x0c000000)
- Quinary bit clock (RCG @ 0x0c032000: CMD +0, CFG +4, M +8, N +0xc, D +0x10; branch CBCR
  @ 0x0c032018). Working divider (ADSP used it for Quaternary, confirmed by read-back):
  CFG 0x2513, M 1, N 0xf8, D 0xf7 -> 1.536 MHz. UPDATE = CMD bit0; CBCR bit0 = enable,
  bit31 = CLK_OFF. **Keep asking the ADSP for this clock** (its clock request works); Linux
  need not touch the RCG if the ADSP already enabled it.
- I2S interface control: `I2S_CTL[n] = 0x0c0c4000 + n*0x1000`, n = 0..6. Reset 0x000f0004.
  Quinary is **slot 5** (0x0c0c9000) — the only slot where a direct write produced WS.
  Working value 0x000f4400 = SPKEN | SPKMODE(SD0) | WS internal | 16-bit (matches ADSP's
  Quaternary I2S_CTL and the mainline lpass-lpaif-reg.h field layout).
- Pad mux: quin_iomux 0x0c052000 bit0 = route Quinary to pri_mi2s pads; gpio92 -> func1
  (pri_mi2s_ws), gpio91 = BCLK, gpio88/93 = data. (mainline apq8016_sbc already writes
  quin_iomux bit0 in its dai_init.)
- LPAIF read-DMA: base 0x0c0d2000, 4 channels stride 0x1000. Per channel: CTL +0, BASE +4,
  BUFF_LEN +8, CURR +0xc, PER_LEN +0x10. CTL fields (from ADSP DMA HAL, mask 0x1bc3e):
  bit0 enable, bits1-5 FIFO watermark, bits10-13 = **audio interface select**, bits15-16
  burst/wps. Live Primary channel 0 CTL read = 0x0000604f.
- DMA interface-select block 0x0c0cf000 (+0x08 seen = 0x00008001 during Primary). **OPEN**:
  exact mapping of channel<->interface here.

## Two implementation approaches

### A. Full Linux LPAIF path (clean, more work)
A small platform driver, modelled on mainline `sound/soc/qcom/lpass-apq8016.c` + `lpass-cpu.c`
+ `lpass-platform.c`, that owns Quinary only:
1. Get the Quinary bit clock (ask the ADSP via the existing q6afe clock API, or drive the RCG).
2. On hw_params: program I2S_CTL[5] and a free read-DMA channel (BASE = DMA buffer phys,
   BUFF_LEN, PER_LEN; CTL interface-select = Quinary).
3. On trigger: enable DMA channel then I2S_CTL SPKEN.
4. tas2560 codec stays on the same DAI link; amp powers up on DAPM as it already does.
Risk: the ADSP also owns this LPAIF block; must pick a DMA channel the ADSP never uses
(channels 1-3 read as idle 0x000f0004 / 0x00000003 while Primary runs on ch0 — candidates).

### B. Redirect an ADSP-driven stream (small, hacky)
Let the ADSP run a normal **Quaternary** playback (that path works end to end), then have a
tiny kernel shim repoint that DMA channel's interface-select field (CTL bits10-13) from
Quaternary to Quinary, and set I2S_CTL[5]. The ADSP keeps feeding DMA; the bytes come out on
Quinary pads. Least code, but relies on the interface-select field being writable behind the
ADSP and on the ADSP not re-checking it.

## DMA↔interface routing block DECODED (2026-09-30, from ADSP firmware seg04)
The msm8953 DMA→interface binding is a separate mux block, NOT the DMA CTL (confirmed:
CTL bits10-13 are words-per-sample). Decode from the firmware I2S/DMA HAL (adsp-fw/dis04.txt):
- LPAIF base is held in ADSP global `0xf0ce9c78` = **0x0c0cf000** (2nd instance in `0xf0ce9c7c`).
  Register `= base + word_off*4`; the word_off global (`0xf0ce9c80`) is 0 on this build.
- **Routing/mux registers: base+0x1004 = 0x0c0d0004, +0x1008 = 0x0c0d0008, +0x100c = 0x0c0d000c.**
- Read-DMA CTL/BASE/BUFF/CURR/PER = base+0x3000+ch*0x1000 = **0x0c0d2000+ch*0x1000** (matches quindma).
- **Getter** `f053613c` reads {0x0c0d0008, 0x0c0d0004} into caller storage (the "current routing").
- **Setter** `f0535ef8` (+parallel `f05362ac` for instance 1, which also ORs 0x1800000) writes a
  table-driven (mask,val) pair to 0x0c0d000c / 0x0c0d0004 via the RMW helper `f05476b4`
  (`reg = (reg & ~mask) | val`, r4=1 ⇒ plain overwrite). The tables (adsp.b04 @ 0xf07b6710..67ac,
  period-4) give **only 4 patterns**, indexed by the port/interface index mod 4:
  - `pattern[0] = 0x00008038` = field bits3-5 + enable bit15
  - `pattern[1] = 0x000101c0` = field bits6-8 + enable bit16
  - `pattern[2] = 0x00020e00` = field bits9-11 + enable bit17
  - `pattern[3] = 0x00000000` = no external mux (internal-only)
- So 0x0c0d000c holds **three 3-bit mux-select fields** (at bit3, bit6, bit9), each with an
  enable (bit15/16/17). Interface→pattern (best inference; interface 3 = internal fits test #28
  "Quaternary drives no external pads"): PRI(0)→pat0, SEC(1)→pat1, TER(2)→pat2, QUAT(3)→none,
  **QUIN→pat0 or pat1** (index 4→pat0 / index 5→pat1 depending on the devcfg numbering 0,1,2,3,5,6).
- Live during earpiece (regsnap `routing-earpiece.bin`, normal OS): 0x0c0d000c=0, 0x0c0d0004=0,
  **0x0c0d0008=0x00008001** (bit0+bit15), **0x0c0d1000=0x00038007** (bits0-2 + bits15-17). Earpiece
  is the WCD internal codec (no I2S interface), so 0x0c0d000c/4 are 0 — consistent.
- **OPEN (needs one reboot-free live test):** which of the 3 fields is Quinary, and the 3-bit value
  to write (the mask sets all 3 bits = 7; the real per-channel value may be the DMA channel index).
  Resolve empirically: program I2S_CTL[5] for the amp's SD lines (SD2/SD3 = QUAD23, SPKMODE=0x6<<10),
  point an idle read-DMA channel at the LLB, sweep 0x0c0d000c through pattern0/1/2 (± field=channel),
  and sample gpio94/95 (amp DATA). Whichever makes gpio94/95 toggle is the Quinary routing value.

## quinroute.ko test RESULT (2026-09-30, normal OS, no reboot)
Built `quinroute/quinroute.ko` and ran it with an earpiece tone active (ch0 read-DMA live:
CTL=0x0000604f, LLB BASE=0x0c0e0020, BUFF=0x5f, PER=0x2f). Swept SPKMODE {SD0,SD2,SD3,QUAD23}
× the 3 routing patterns written to **0x0c0d000c**, on an idle DMA channel (ch1) pointed at the
earpiece LLB. **RESULT: none bound the DMA to Quinary** — in every combo the DMA CURR advanced
exactly one FIFO depth (to 0x0c0e0040 = base+0x20 = 8 words) then STALLED, and NO amp data pad
(gpio88/93/94/95) toggled. Same wall quindma hit: the interface does not drain the DMA.
⇒ **0x0c0d000c is NOT the read-DMA→interface binding for playback.** Key evidence from the run:
during working earpiece playback the nonzero routing reg is **0x0c0d0008 = 0x08008001** (bit0,
bit15, bit27) and **0x0c0d1000 = 0x00038007** (bits0-2 + bits15-17), while 0x0c0d000c stays 0.
So the read-DMA→interface routing lives in **0x0c0d0008** (and possibly the 0x0c0d1000 enable),
not 0x0c0d000c. Firmware field hint (descriptor fn f0182078) for 0x0c0d0008: fields at bits8-11
(0xf00), bit16, bits24-31. NEXT (needs care, not a blind poke): RE the 0x0c0d0008 SETTER and how
the DMA-channel index + interface index encode into it; only then test. A blind 0x0c0d0008 sweep
is discouraged (guess-and-test). Alternatively the Android APR capture (ANDROID-APR-CAPTURE.md)
gives the proper q6afe fix without any of this.

## Quaternary capture + quinroute v2/v3 (2026-09-30 session 5) — new facts, still stuck
Test boot (quat-ramboot.img) captured the routing block 0x0c0d0000..0x0c0d3000 in idle /
working-Quaternary / frozen-Quinary states (regsnap rt-idle/quat/quin.bin):
- **Working Quaternary:** read-DMA ch0 CTL=**0x610f**, BASE=0x0c0e0020(LLB), and it DRAINS
  (CURR advanced to 0x130). Routing: **0x0c0d0008=0x1, 0x0c0d1000=0x7**, and 0x0c0d0004/000c=0.
- **Frozen Quinary:** whole block reads 0x210f = ADSP already dead (expected).
- **KEY DISCOVERY — DMA CTL interface-select is bits 6-8** (mask 0x1c0), which every prior
  test left at 0: earpiece/internal-codec=1, **Quaternary=4** ⇒ value looks like I2S_slot+1
  (Primary slot0→1, Quat slot3→4). Predicted Quinary(slot5)=6 (or MI2S-index 5). This is the
  field quindma's bits10-13 sweep missed (bits10-13 = words-per-sample, confirmed).
- Quinary clock block during a Quinary attempt: IBIT_CBCR=0x1 (on), OSR/EBIT off — so no extra
  interface clock is missing; IBIT alone is what a running interface uses.
quinroute v2/v3 (normal OS, DMA-first order, proven I2S_CTL5=0x000f4400 SD0, swept intf 1-7):
- **The Quinary interface IS alive under Linux** — WS(gpio92) toggles ~2230 edges/sample window.
- **But the read-DMA never drains** into it: CURR fills exactly one FIFO (8 words, to base+0x20)
  then stalls, for EVERY interface-select value, and NO data appears on any amp pad. Same wall
  as quindma/v1. So driving DMA + interface-select + interface-clock from Linux is NOT enough to
  make the interface consume the DMA. (Note: quindma's earlier "feasibility proven / CURR
  advances" was only this same one-FIFO fill — the DMA has NEVER continuously drained under Linux.)

**HONEST STATUS:** after 3 sessions the Linux bypass still cannot get the interface to consume the
DMA. What's missing is the actual DMA↔interface drain handshake, which can't be disentangled from
the data we can capture (earpiece and Quaternary both use ch0, so channel-vs-interface encoding is
ambiguous; only the DSP-driven port-start establishes a draining connection). Register-poking has
hit diminishing returns. The **Android APR capture** (ANDROID-APR-CAPTURE.md) is the method that
resolves exactly this class (it cracked Fairphone 3/5) — it needs an Android boot (blocked: eMMC).

## Decision point / next data needed (OPEN)
1. Confirm the CTL interface-select encoding: read a live Quaternary DMA channel's CTL and
   compare to Primary's (both known-good), to learn the field value for each interface, then
   infer Quinary's. (Read-only, normal OS — same method as the Primary read already done.)
2. Confirm which read-DMA channel index is free for Linux to use.
3. Then prototype B first (fewest moving parts): one test that plays Quaternary via the ADSP,
   flips CTL interface-select + I2S_CTL[5], and samples pads for WS on gpio92 + data. If sound
   comes out the bottom speaker, done. If not, fall back to A.
