#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Native x86 build of the patched tree, only to run the AF / focus-stats unit tests on the PC.
set -eu
apk add -q clang lld meson ninja pkgconf python3 py3-jinja2 py3-ply py3-yaml openssl coreutils \
	musl-dev g++ linux-headers eudev-dev gnutls-dev libevent-dev libyuv libyuv-dev yaml-dev \
	mesa-dev libjpeg-turbo-dev
rm -rf /b && cp -a /w/src /b && cd /b
meson setup t -Dpipelines=simple -Dipas=simple -Dtest=true -Dcam=disabled -Dqcam=disabled \
	-Dgstreamer=disabled -Dv4l2=false -Dlc-compliance=disabled -Dpycamera=disabled \
	-Ddocumentation=disabled -Dwerror=false >/dev/null
ninja -C t test/simple-ipa-af test/swstats-cpu-focus
meson test -C t --no-rebuild --print-errorlogs simple-ipa-af swstats-cpu-focus
