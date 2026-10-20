#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# decomp.sh ADDR... : decompile (creating functions if needed) from the saved project; prints C.
D="$(cd "$(dirname "$0")" && pwd)"
OUT=/work/out/decomp-$$.c
docker run --rm --user "$(id -u):$(id -g)" -e GHIDRA_HEADLESS_MAXMEM=8G \
  -v "${GHIDRA:-$D/ghidra_12.1.4_PUBLIC}:/opt/ghidra:ro" -v "$D/scripts:/scripts:ro" -v "$D/work:/work" \
  rea-ghidra /opt/ghidra/support/analyzeHeadless /work/proj oxygen-adsp -process adsp.elf -noanalysis \
    -scriptPath /scripts -postScript Decomp.java "$OUT" "$@" >"$D/work/out/decomp-last.log" 2>&1
cat "$D/work/out/decomp-$$.c"; rm -f "$D/work/out/decomp-$$.c"
