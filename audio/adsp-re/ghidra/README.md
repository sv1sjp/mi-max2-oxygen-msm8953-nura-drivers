# adsp-re/ghidra/: decompile the ADSP firmware and decode its live debug log

The tools that found the loudspeaker fix. Bring your own firmware: the ADSP image
(`adsp.mdt` + `adsp.b00`..`b13`) from the phone's modem partition, and a rebuilt ELF
`adsp.elf` of it, in `fw/` next to these scripts (or point `FW=` at it).

## Ghidra in Docker (no installs on the host)

1. Download Ghidra 12.1.4 from the NSA release page, check its SHA-256, unzip it here
   (`ghidra_12.1.4_PUBLIC/`, or set `GHIDRA=`).
2. `docker build -t rea-ghidra .` (Ubuntu + JDK 21 + Node 24 + the `rea-agents` CLI;
   [REA](https://github.com/morluto/rea) is optional, `rea.sh` runs it).
3. `./analyze-adsp.sh`: one-time import + auto-analysis (~11 min for 7 MB of Hexagon code)
   into `work/proj`, and the whole firmware decompiled to `work/out/adsp.c`.
4. `python3 annotate.py`: `adsp-annot.c` with every diag message text next to its use.

Then:
- `python3 fn.py ADDR|FUN_xxxxxxxx` prints a function, `fn.py -x NAME` its callers;
- `./decomp.sh ADDR...` creates and decompiles functions that Ghidra missed (jump-table
  targets) from the saved project;
- `python3 walk.py ROOT DEPTH [skip...]` prints a call tree with loops and messages.

## Live ADSP log

`../../drivers/adspdiag/` is a small kernel module that talks DIAG to the ADSP (use it
instead of `rpmsg_char`, which panics the kernel on this channel). Capture with
`adspdiag-raw.py OUT seconds`, then on the PC:

```sh
python3 diagdec.py capture.bin              # timeline
python3 diagdec.py capture.bin --summary    # counts per message
```

`0x79` messages carry their text. QShrink `0x92` messages only carry a hash; `diagdec.py`
finds the matching `{line, ssid, mask, hash}` constant in the firmware and the function
that logs it (from `adsp-annot.c`, or the `##0x` constants of an `llvm-objdump`
disassembly `fw/a04.txt` as a fallback).
