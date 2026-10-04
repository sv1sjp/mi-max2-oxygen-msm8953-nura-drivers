#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# collect.sh <libcamera DESTDIR tree | -> <out> [--no-af]
# Copy only the files install.sh replaces on the phone (the library, the simple IPA module +
# its signature, the IPA proxy, cam) and add our tuning files, laid out like the phone's
# filesystem. "-" instead of a tree: tuning files only (Nura's own libcamera stays).
# --no-af: leave the Af block out of imx386.yaml, for a libcamera without the autofocus
# patches (it would reject the whole file). Used by xbuild.sh and build-on-phone.sh.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
D=$1; O=$2; NOAF=${3:-}
rm -rf "$O"
if [ "$D" != - ]; then
	for f in "$D"/usr/lib/libcamera.so.*.*.* "$D"/usr/lib/libcamera-base.so.*.*.*; do
		install -D -m755 "$f" "$O/usr/lib/${f##*/}"
	done
	install -D -m755 "$D/usr/lib/libcamera/ipa/ipa_soft_simple.so" "$O/usr/lib/libcamera/ipa/ipa_soft_simple.so"
	install -D -m644 "$D/usr/lib/libcamera/ipa/ipa_soft_simple.so.sign" "$O/usr/lib/libcamera/ipa/ipa_soft_simple.so.sign"
	install -D -m755 "$D/usr/libexec/libcamera/soft_ipa_proxy" "$O/usr/libexec/libcamera/soft_ipa_proxy"
	install -D -m755 "$D/usr/bin/cam" "$O/usr/bin/cam"
fi
Y=$O/usr/share/libcamera/ipa/simple
mkdir -p "$Y"
install -m644 "$HERE/s5k5e8.yaml" "$Y/s5k5e8.yaml"
if [ "$NOAF" = --no-af ]; then
	# drop from "  - Af:" up to the next algorithm ("  - ...")
	awk '/^  - Af:/ { skip = 1; next } skip && /^  - / { skip = 0 } !skip' "$HERE/imx386.yaml" > "$Y/imx386.yaml"
	chmod 644 "$Y/imx386.yaml"
else
	install -m644 "$HERE/imx386.yaml" "$Y/imx386.yaml"
fi
