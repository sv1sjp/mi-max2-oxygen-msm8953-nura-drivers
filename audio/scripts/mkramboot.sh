#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# usage: mkramboot.sh <dtb> <out.img> [extra cmdline, e.g. "module_blacklist=q6afe"]
# vmlinuz/initramfs = copies of the phone's /boot files in the current folder, e.g.
#   ssh user@172.16.42.1 'cat /boot/vmlinuz' > vmlinuz ; same for initramfs)
set -eu
DTB=$(readlink -f "$1"); OUT=$(readlink -f "$2"); EXTRA=${3:-}
W=$(mktemp -d); cat vmlinuz "$DTB" > $W/k; cp initramfs $W/
docker run --rm -v "$W:/s" -v "$(dirname "$OUT"):/o" ubuntu:latest bash -c "apt-get update -qq >/dev/null; apt-get install -y -qq mkbootimg >/dev/null 2>&1; mkbootimg --header_version 0 --pagesize 0x800 --base 0 --kernel_offset 0x80008000 --ramdisk_offset 0x81000000 --tags_offset 0x80000100 --kernel /s/k --ramdisk /s/initramfs --cmdline 'loglevel=7 $EXTRA pmos_boot_uuid=06bdd9ad-8ba6-4c6d-bf31-831001e01a5e pmos_root_uuid=4a9e7036-f754-4d61-b4e1-fcc4e4d4a269 pmos_rootfsopts=defaults' -o /o/$(basename "$OUT") && chown 1000:1000 /o/$(basename "$OUT")"
rm -rf $W
