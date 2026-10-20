#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# One-time: import + auto-analyse the oxygen ADSP firmware into a saved Ghidra
# project (work/proj) and export decompiled C to work/out/.
D="$(cd "$(dirname "$0")" && pwd)"
exec docker run --rm --user "$(id -u):$(id -g)" \
  -e GHIDRA_HEADLESS_MAXMEM=12G \
  -v "${GHIDRA:-$D/ghidra_12.1.4_PUBLIC}:/opt/ghidra:ro" \
  -v "${FW:-$D/fw}:/fw:ro" \
  -v "$D/scripts:/scripts:ro" \
  -v "$D/work:/work" \
  rea-ghidra /opt/ghidra/support/analyzeHeadless /work/proj oxygen-adsp \
    -import /fw/adsp.elf -overwrite -max-cpu 12 \
    -scriptPath /scripts -postScript ExportAllC.java /work/out \
    -log /work/out/headless.log
