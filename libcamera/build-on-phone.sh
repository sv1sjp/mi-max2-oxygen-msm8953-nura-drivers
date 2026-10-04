#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the patched libcamera on the phone, for the libcamera version the phone has
# (install.sh --newversion runs this for you). Needs internet and ~2 GB free in /var/tmp,
# takes roughly 30-60 minutes.    sudo sh build-on-phone.sh
# Output: ./out/root/, only the files install.sh replaces (see collect.sh).
#
# The base is Nura's own libcamera package of this release: the same upstream source and
# the same Nura patches, taken from pmaports and checked against its checksums. On top come
# our patches 0004-0014 (autofocus, IMX386/S5K5E8 support, faster exposure). 0001-0003 are
# Nura's v26.06 package patches; they are used only if the package recipe is not found.
# The build tools are installed for the time of the build only (apk virtual package
# .mi-max-2-lcbuild).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
PMAPORTS=https://gitlab.postmarketos.org/postmarketOS/pmaports/-/raw
UPSTREAM=https://gitlab.freedesktop.org/camera/libcamera/-/archive
WORK=${WORK:-/var/tmp/mi-max-2-mainline-lcbuild}
DEPS="build-base meson pkgconf python3 py3-jinja2 py3-ply py3-yaml openssl coreutils curl patch
	linux-headers eudev-dev gnutls-dev libevent-dev libunwind-dev libyuv libyuv-dev yaml-dev
	mesa-dev libdrm-dev libjpeg-turbo-dev"
say() { echo "== $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }
ls "$HERE"/patches/0004-*.patch >/dev/null 2>&1 || die "third-party patches missing: run sh ../fetch-upstream.sh first"

[ "$(id -u)" = 0 ] || die "run as root"
cleanup() { rm -rf "$WORK"; apk del -q .mi-max-2-lcbuild >/dev/null 2>&1 || true; }
trap cleanup EXIT
mkdir -p "$WORK"
[ "$(df -Pk "$WORK" | awk 'NR == 2 { print $4 }')" -gt 2000000 ] || die "need ~2 GB free in $WORK"

# The version the phone has: the libcamera.so.X.Y.Z that belongs to the installed package
if [ -z "${VERSION:-}" ]; then
	for f in /usr/lib/libcamera.so.*.*.*; do
		apk info -q --who-owns "$f" >/dev/null 2>&1 && VERSION=${f##*/libcamera.so.}
	done
fi
[ -n "${VERSION:-}" ] || die "no libcamera package found (apk add libcamera libcamera-ipa)"

say "installing the build tools (removed again at the end)"
# shellcheck disable=SC2086
apk add -q --virtual .mi-max-2-lcbuild $DEPS

cd "$WORK"
branch=$(sed -n 's/^VERSION_ID="\{0,1\}\(v[0-9][0-9]\.[0-9][0-9]\)"\{0,1\}$/\1/p' "${OS_RELEASE:-/etc/os-release}")
found=""
for b in ${branch:-} main; do
	curl -sSfL -o APKBUILD "$PMAPORTS/$b/temp/libcamera/APKBUILD" || continue
	[ "$(sed -n 's/^_pkgver=//p' APKBUILD)" = "$VERSION" ] || continue
	found=$b; break
done
base=""
T=libcamera-v$VERSION.tar.gz
curl -sSfL -o "$T" "$UPSTREAM/v$VERSION/$T"
mkdir patches
if [ -n "$found" ]; then
	say "libcamera $VERSION: Nura package patches from pmaports $found"
	# every file of the recipe's source list that ends in .patch, in order
	sed -n '/^source="/,/"$/p' APKBUILD | tr -d '"' | sed 's/^source=//' | tr -s ' \t' '\n\n' \
		| grep '\.patch$' > patches.list || true
	while read -r p; do
		curl -sSfL -o "patches/$p" "$PMAPORTS/$found/temp/libcamera/$p"
	done < patches.list
	# checksums of the tarball and the patches, from the recipe
	sed -n '/^sha512sums="/,/"$/p' APKBUILD | tr -d '"' | sed 's/^sha512sums=//' \
		| awk 'NF == 2' > sums
	awk -v f="$T" '$2 == f' sums > sums.check
	(cd patches && while read -r p; do awk -v f="$p" '$2 == f { print $1 "  patches/" f }' ../sums; done < ../patches.list) >> sums.check
	[ "$(wc -l < sums.check)" = "$(($(wc -l < patches.list) + 1))" ] || die "Nura's recipe has no checksum for some files"
	sha512sum -c sums.check >/dev/null || die "the download does not match Nura's checksums"
	base=""
else
	echo "WARNING: libcamera $VERSION not found in pmaports: using our copies of Nura's v26.06 patches" >&2
	base=$(ls "$HERE"/patches/000[123]-*.patch)
fi

# What we add, from most to least: everything (with autofocus), or only the sensor support
# (correct gain and black level for both cameras). The autofocus patches are the most likely
# to need updating for a new libcamera; then the build falls back to the next level.
full=""; for p in "$HERE"/patches/*.patch; do case "$p" in */000[123]-*) ;; *) full="$full $p" ;; esac; done
sensor=$(ls "$HERE"/patches/0010-*.patch "$HERE"/patches/0014-*.patch)
agc=$(ls "$HERE"/patches/0013-*.patch)

prepare() {  # fresh source tree with Nura's patches
	cd "$WORK" && rm -rf src && mkdir src && tar xzf "$T" -C src --strip-components=1 && cd src
	for p in $(cat ../patches.list 2>/dev/null) $base; do
		case "$p" in /*) ;; *) p=../patches/$p ;; esac
		patch -p1 -s < "$p" >/dev/null || die "Nura patch $(basename "$p") does not apply"
	done
}
apply() {  # apply <patches>: all of them, or fail (a patch that is already in is skipped)
	for p in "$@"; do
		if patch -p1 -N -s --dry-run < "$p" >/dev/null 2>&1; then
			patch -p1 -N -s < "$p"
		elif patch -p1 -R -s --dry-run < "$p" >/dev/null 2>&1; then
			echo "already in libcamera $VERSION: $(basename "$p")"
		else
			echo "does not apply to libcamera $VERSION: $(basename "$p")"; return 1
		fi
	done
}
build() {
	# about 1 GB of memory per compile job
	jobs=$(awk '/^MemTotal/ { j = int($2 / 1000000); print (j < 1) ? 1 : j }' /proc/meminfo)
	[ "$jobs" -gt "$(nproc)" ] && jobs=$(nproc)
	CFLAGS=-O2 CXXFLAGS=-O2 meson setup build \
		--prefix=/usr --libdir=lib --libexecdir=libexec --sysconfdir=/etc --buildtype=plain \
		-Dpipelines=simple,uvcvideo,virtual -Dipas=simple -Dcam=enabled -Dv4l2=false \
		-Dgstreamer=disabled -Dqcam=disabled -Dlc-compliance=disabled -Dpycamera=disabled \
		-Ddocumentation=disabled -Dtest=false -Dwerror=false >../meson.log 2>&1 \
		|| { tail -20 ../meson.log; return 1; }
	ninja -C build -j"$jobs" || return 1
	rm -rf "$WORK/dest"
	DESTDIR=$WORK/dest meson install --no-rebuild -C build >/dev/null || return 1
	# strip, then re-sign the IPA module (the signature covers the stripped file), like Nura's package
	find "$WORK/dest" -type f | while read -r f; do
		if [ "$(head -c 4 "$f" | od -An -c | tr -d ' ')" = '177ELF' ]; then strip --strip-unneeded "$f"; fi
	done
	key=$(find build -type f -name '*ipa-priv-key.pem' | head -n1)
	for ipa in "$WORK"/dest/usr/lib/libcamera/ipa/ipa*.so; do
		src/ipa/ipa-sign.sh "$key" "$ipa" "$ipa.sign" || return 1
	done
}

level=none
# shellcheck disable=SC2086
for try in full sensor; do
	prepare
	if [ $try = full ]; then apply $full || continue; say "building libcamera $VERSION with all our patches (the long part)"
	else
		apply $sensor || continue
		apply $agc || echo "(continuing without the faster exposure patch)"
		say "building libcamera $VERSION with the sensor support only, without autofocus"
	fi
	if build; then level=$try; break; fi
	echo "WARNING: the $try build failed" >&2
done

case $level in
full) sh "$HERE/collect.sh" "$WORK/dest" "$HERE/out/root" ;;
sensor) sh "$HERE/collect.sh" "$WORK/dest" "$HERE/out/root" --no-af ;;
none)
	echo "WARNING: our libcamera patches do not fit libcamera $VERSION: keeping Nura's libcamera," >&2
	echo "         only the tuning files are installed (cameras work, without our improvements)" >&2
	sh "$HERE/collect.sh" - "$HERE/out/root" --no-af ;;
esac
echo "$level" > "$HERE/out/level"
chown -R "$(stat -c %u:%g "$HERE")" "$HERE/out"
say "done ($level): $HERE/out/root"
find "$HERE/out/root" -type f | sort
