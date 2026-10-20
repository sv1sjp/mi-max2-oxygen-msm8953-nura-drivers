#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Run the REA CLI in the container. Firmware dir is mounted read-only at /fw,
# results go to ./work (mounted rw at /work).
D="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$D/work/.home"
exec docker run --rm -i --user "$(id -u):$(id -g)" \
  -v "${GHIDRA:-$D/ghidra_12.1.4_PUBLIC}:/opt/ghidra:ro" \
  -v "${FW:-$D/fw}:/fw:ro" \
  -v "$D/work:/work" \
  ${REA_ENV:+$REA_ENV} rea-ghidra "$@"
