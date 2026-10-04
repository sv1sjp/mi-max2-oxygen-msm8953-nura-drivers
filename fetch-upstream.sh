#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Download the third-party files this project builds on. They are not stored in this
# repository: they stay in their authors' repositories. Only needed to REBUILD (install.sh
# uses the prebuilt files and does not need this). Every download is pinned and checked.
#     sh fetch-upstream.sh
#
#   libcamera/patches/0001-0003  Robert Mader, from postmarketOS pmaports (v26.06)
#   libcamera/patches/0004-0009  Kostiantyn Andriiuk, postmarketos-xiaomi-vince
#   kernel/torch/leds-qcom-pmi8950-torch.c
#                                the same author's PMI8950 torch driver, taken out of his
#                                kernel patch 0014, then our optional-label patch applied
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
VINCE=https://raw.githubusercontent.com/kotXio/postmarketos-xiaomi-vince/8632bfaa7f0dfdb35e720bb736beda19aafdfb46/patches
PMA=https://gitlab.postmarketos.org/postmarketOS/pmaports/-/raw/v26.06/temp/libcamera
die() { echo "ERROR: $*" >&2; exit 1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

# get <url> <sha256> <destination>
get() {
	curl -sSfL -o "$T/dl" "$1" || die "download failed: $1"
	echo "$2  $T/dl" | sha256sum -c - >/dev/null 2>&1 || die "checksum mismatch (upstream changed?): $1"
	mv "$T/dl" "$3"; echo "ok  ${3#"$HERE"/}"
}

P=$HERE/libcamera/patches; mkdir -p "$P"
get $PMA/0001-libcamera-simple-Enable-softISP-for-the-Pinephone.patch 2b6d0012cb8dfc9c73db808536cefffe40b031b9d25643173fd1dec8b0e9b3b7 "$P/0001-libcamera-simple-Enable-softISP-for-the-Pinephone.patch"
get $PMA/0002-libcamera-simple-Skip-hwISP-formats-if-swISP-is-acti.patch ce0137349a4717e1ddb3392c41f8f75e42c80e5e4611357c6d2ff09040d3a576 "$P/0002-libcamera-simple-Skip-hwISP-formats-if-swISP-is-acti.patch"
get $PMA/0003-RFC-egl-Implement-DMABuf-import-for-input-buffers.patch 70636cab55ae84d8befdabd3050d2838afbe7653c4c61d4cc3c0e42bb6d61270 "$P/0003-RFC-egl-Implement-DMABuf-import-for-input-buffers.patch"
get $VINCE/libcamera/0001-simple-raw-lens-plumbing.patch 60f5164e2e1ebace2014084d6014ff4dd998a6535490bee2592b04e7953e89e0 "$P/0004-pipeline-simple-add-guarded-raw-lens-plumbing.patch"
get $VINCE/libcamera/0002-software-isp-add-focus-statistics.patch aab51d0dddd73988b680279e24c9b2f1659b2434d049296bd9d472c2729a99ec "$P/0005-Add-CPU-focus-statistics.patch"
get $VINCE/libcamera/0003-simple-ipa-bounded-one-shot-af.patch 322cd744c20bf134415bfceae62216d064e7a60b50436e3a9ab4e4e63bf56508 "$P/0006-Add-bounded-Simple-IPA-autofocus.patch"
get $VINCE/libcamera/0004-simple-ipa-advertise-afstate.patch cf065ef2c12a4227028e7a5aac19a829533c918cd68777ca8b5ce2ae6390c52e "$P/0007-ipa-simple-advertise-AfState-control.patch"
get $VINCE/libcamera/0005-software-isp-guard-late-lens-callbacks.patch 6b8ca16cfbd5062da10626656338abf322ffbb2302a8e69b8c35c9fea09b80d0 "$P/0008-software-guard-late-lens-callbacks-during-stop.patch"
get $VINCE/libcamera/0006-ipa-simple-guard-malformed-sensor-controls.patch 1353fb8148d3fd3fe20914e9ba505b7ba9d4c1b935c443494b7a56e0dd2de1a4 "$P/0009-ipa-simple-guard-malformed-sensor-controls.patch"

# torch driver: the new file inside upstream's kernel patch 0014, plus our label patch
get $VINCE/kernel/0014-pmi8950-dual-color-torch.patch cf426d4239d62f5991a0ff35f85938c19426e7e9cc17c34faf6f02d1674b7275 "$T/0014.patch"
mkdir -p "$T/k/drivers/leds"
awk '/^diff --git a\/drivers\/leds\/leds-qcom-pmi8950-torch.c/ { f = 1; next }
     f && /^diff --git/ { exit }
     f && /^\+\+\+/ { p = 1; next }
     f && p && /^\+/ { print substr($0, 2) }' "$T/0014.patch" > "$T/k/drivers/leds/leds-qcom-pmi8950-torch.c"
(cd "$T/k" && patch -p1 -s < "$HERE/kernel/torch/0001-leds-pmi8950-torch-optional-label.patch") || die "label patch does not apply"
echo "68a54fce638483156c5a2cee811b4b21c6bf1c751f3c29fbbfac7602a73e5e3d  $T/k/drivers/leds/leds-qcom-pmi8950-torch.c" | sha256sum -c - >/dev/null 2>&1 || die "generated torch driver differs from the tested one"
cp "$T/k/drivers/leds/leds-qcom-pmi8950-torch.c" "$HERE/kernel/torch/leds-qcom-pmi8950-torch.c"; echo "ok  kernel/torch/leds-qcom-pmi8950-torch.c"
