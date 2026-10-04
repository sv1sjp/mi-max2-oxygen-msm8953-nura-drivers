#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build imx386.ko, s5k5e8.ko, dw9768.ko (with the DW9763 compatible), the fixed qcom-camss.ko and
# leds-qcom-pmi8950-torch.ko, plus the three DTBs (camera, led, camera-led), for the Nura
# (postmarketOS) kernel linux-postmarketos-qcom-msm8953. Output: ./out/<kernel release>/
#
# On the phone, for the kernel it runs (install.sh --newversion does this for you; needs
# internet and ~3 GB free in /var/tmp, takes roughly 20-40 minutes):
#     sudo sh build-modules.sh --phone
#   It takes the kernel source tag and checksum from Nura's kernel package (pmaports),
#   the configuration from the running kernel (/proc/config.gz), installs the build tools
#   for the time of the build only (apk virtual package .mi-max-2-kbuild), and prepares
#   the kernel tree instead of building the whole kernel.
#
# On a PC with Docker, full kernel build (~15 min on 16 cores). This reproduces prebuilt/kernel/:
#     docker run --rm -v "$PWD:/w" alpine:3.24 sh /w/build-modules.sh
#   Another kernel: pass its msm8953-mainline tag and its config (zcat /proc/config.gz > config-<version>):
#     docker run --rm -e TAG=7.1.3-r0 -e CONFIG=config-7.1.3 -v "$PWD:/w" alpine:3.24 sh /w/build-modules.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
PHONE=0; [ "${1:-}" = --phone ] && PHONE=1
PMAPORTS=https://gitlab.postmarketos.org/postmarketOS/pmaports/-/raw
KPKG=linux-postmarketos-qcom-msm8953
DEPS="clang lld llvm make bison flex perl python3 openssl openssl-dev linux-headers musl-dev
	binutils gcc elfutils-dev pahole curl patch kmod findutils"
say() { echo "== $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }
[ -f "$HERE/torch/leds-qcom-pmi8950-torch.c" ] || die "third-party torch driver missing: run sh ../fetch-upstream.sh first"

if [ $PHONE = 1 ]; then
	[ "$(id -u)" = 0 ] || die "run as root"
	WORK=${WORK:-/var/tmp/mi-max-2-mainline-kbuild}
	KREL=${KREL:-$(uname -r)}
	KCONFIG=${KCONFIG:-/proc/config.gz}
	cleanup() { rm -rf "$WORK"; apk del -q .mi-max-2-kbuild >/dev/null 2>&1 || true; }
	trap cleanup EXIT
	mkdir -p "$WORK"
	[ "$(df -Pk "$WORK" | awk 'NR == 2 { print $4 }')" -gt 3000000 ] || die "need ~3 GB free in $WORK"
	say "installing the build tools (removed again at the end)"
	# shellcheck disable=SC2086
	apk add -q --virtual .mi-max-2-kbuild $DEPS
	if [ -z "${TAG:-}" ]; then
		# The kernel package builds the msm8953-mainline tag _tag (usually <version>-r0).
		# Read it and the source checksum from the package recipe of this release.
		kver=${KREL%%-*}
		branch=$(sed -n 's/^VERSION_ID="\{0,1\}\(v[0-9][0-9]\.[0-9][0-9]\)"\{0,1\}$/\1/p' "${OS_RELEASE:-/etc/os-release}")
		for b in ${branch:-} main; do
			curl -sSfL -o "$WORK/APKBUILD" "$PMAPORTS/$b/device/community/$KPKG/APKBUILD" || continue
			pkgver=$(sed -n 's/^pkgver=//p' "$WORK/APKBUILD")
			[ "$pkgver" = "$kver" ] || continue
			TAG=$(sed -n 's/^_tag="\(.*\)"$/\1/p' "$WORK/APKBUILD" | sed "s/\$pkgver/$pkgver/")
			SHA512=$(awk -v f="$KPKG-v$TAG.tar.gz" '$2 == f { print $1 }' "$WORK/APKBUILD")
			say "kernel $kver: source tag v$TAG (pmaports $b)"
			break
		done
		if [ -z "${TAG:-}" ]; then
			TAG=$kver-r0
			echo "WARNING: kernel $kver not found in pmaports, trying tag v$TAG without checksum" >&2
		fi
	fi
	case "$KCONFIG" in *.gz) zcat "$KCONFIG" ;; *) cat "$KCONFIG" ;; esac > "$WORK/config"
	CONFIG_FILE=$WORK/config
else
	WORK=${WORK:-/build}
	TAG=${TAG:-7.0.9-r0}
	CONFIG_FILE=$HERE/${CONFIG:-config-7.0.9-msm8953}
	[ "$TAG" = 7.0.9-r0 ] && SHA512=660bb93c2d19efe27a0c19a795801cbbf092f19589b96a857e320345b406a887498a0ab6b0e435b3ef5c244f5c193f82c8a61876853f3a6c1cf0932632d00c5f
	# shellcheck disable=SC2086
	apk add -q $DEPS
fi

say "downloading kernel source v$TAG"
mkdir -p "$WORK"; cd "$WORK"
curl -sSfL -o linux.tar.gz "https://github.com/msm8953-mainline/linux/archive/v$TAG.tar.gz"
if [ -n "${SHA512:-}" ]; then echo "$SHA512  linux.tar.gz" | sha512sum -c -; fi
rm -rf "linux-$TAG" obj m; tar xzf linux.tar.gz; rm linux.tar.gz
K=$WORK/linux-$TAG; cd "$K"

say "applying patches"
for p in "$HERE"/patches/*.patch; do
	if patch -p1 -N -s --dry-run < "$p" >/dev/null 2>&1; then
		patch -p1 -N -s < "$p"
	elif patch -p1 -R -s --dry-run < "$p" >/dev/null 2>&1; then
		echo "already in kernel $TAG: $(basename "$p")"
	else
		die "$(basename "$p") does not apply to kernel $TAG and needs updating"
	fi
done
cp "$HERE"/dts/msm8953-xiaomi-oxygen-*.dts* arch/arm64/boot/dts/qcom/
mkdir -p "$WORK/obj" && cp "$CONFIG_FILE" "$WORK/obj/.config"
M="make ARCH=arm64 LLVM=1 O=$WORK/obj -j$(nproc)"
$M -s olddefconfig
KREL_BUILT=$($M -s kernelrelease)
if [ $PHONE = 1 ]; then
	[ "$KREL_BUILT" = "$KREL" ] || die "this source gives kernel $KREL_BUILT, the phone runs $KREL"
fi

if [ $PHONE = 1 ] && ! grep -q '^CONFIG_MODVERSIONS=y' "$WORK/obj/.config"; then
	# Without symbol versions a module only has to match the kernel release and config,
	# so preparing the tree is enough: no need to build the whole kernel on the phone.
	say "preparing the kernel tree"
	$M -s modules_prepare scripts_dtc
	export KBUILD_MODPOST_WARN=1	# no Module.symvers without a full build
else
	say "building the kernel"
	$M Image modules dtbs
fi

say "building the modules"
mkdir -p "$WORK/m/camss" "$WORK/m/dw9768" "$WORK/m/imx386" "$WORK/m/s5k5e8" "$WORK/m/torch"
cp drivers/media/platform/qcom/camss/* "$WORK/m/camss/"
cp drivers/media/i2c/dw9768.c "$WORK/m/dw9768/" && echo 'obj-m += dw9768.o' > "$WORK/m/dw9768/Makefile"
cp "$HERE"/imx386/* "$WORK/m/imx386/"
cp "$HERE"/s5k5e8/* "$WORK/m/s5k5e8/"
cp "$HERE/torch/leds-qcom-pmi8950-torch.c" "$HERE/torch/Makefile" "$WORK/m/torch/"
for d in camss dw9768 imx386 s5k5e8 torch; do $M M="$WORK/m/$d" modules; done

OUT=$HERE/out/$KREL_BUILT
rm -rf "$OUT" && mkdir -p "$OUT"
say "building the device trees"
for v in camera led camera-led; do
	cpp -nostdinc -I include -I arch/arm64/boot/dts -I arch/arm64/boot/dts/qcom \
		-I scripts/dtc/include-prefixes -undef -D__DTS__ -x assembler-with-cpp \
		arch/arm64/boot/dts/qcom/msm8953-xiaomi-oxygen-$v.dts \
		| "$WORK/obj/scripts/dtc/dtc" -q -I dts -O dtb -o "$OUT/$v.dtb" -
done
cp "$WORK/m/camss/qcom-camss.ko" "$WORK/m/dw9768/dw9768.ko" "$WORK/m/imx386/imx386.ko" \
	"$WORK/m/s5k5e8/s5k5e8.ko" "$WORK/m/torch/leds-qcom-pmi8950-torch.ko" "$OUT/"
chown -R "$(stat -c %u:%g "$HERE")" "$HERE/out"
cd "$OUT" && sha256sum -- * && modinfo -F vermagic imx386.ko
say "done: $OUT"
