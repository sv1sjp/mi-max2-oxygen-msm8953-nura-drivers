#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Cross-build the patched libcamera from ./src (prepare-src.sh) for aarch64.
# Run after prepare-src.sh:  docker run --rm -u 0 -v "$PWD:/w" alpine:3.24 sh /w/xbuild.sh
# For a newer Nura release, use the Alpine release it is based on, e.g. alpine:3.25 and
# -e ALPINE=v3.25, so the libraries match the phone.
# Result: /w/out/root (the files install.sh replaces, used by install.sh --newversion) and
# /w/out/meson-summary.txt
set -eu
R=http://dl-cdn.alpinelinux.org/alpine/${ALPINE:-v3.24}
apk add -q clang lld llvm meson ninja pkgconf python3 py3-jinja2 py3-ply py3-yaml openssl coreutils git

mkdir -p /sysroot/etc/apk
apk add -q --root /sysroot --arch aarch64 --initdb --no-scripts --keys-dir /usr/share/apk/keys/aarch64 \
	-X $R/main -X $R/community \
	musl-dev libgcc gcc g++ linux-headers \
	eudev-dev glib-dev gnutls-dev gst-plugins-base-dev libevent-dev libunwind-dev \
	libyuv libyuv-dev yaml-dev mesa-dev libdrm-dev libjpeg-turbo-dev

cat > /cross.txt <<'X'
[binaries]
c = ['clang', '--target=aarch64-alpine-linux-musl', '--sysroot=/sysroot']
cpp = ['clang++', '--target=aarch64-alpine-linux-musl', '--sysroot=/sysroot']
c_ld = 'lld'
cpp_ld = 'lld'
ar = 'llvm-ar'
strip = 'llvm-strip'
pkg-config = 'pkgconf'

[properties]
sys_root = '/sysroot'
pkg_config_libdir = ['/sysroot/usr/lib/pkgconfig', '/sysroot/usr/share/pkgconfig']

[built-in options]
c_args = ['-O2']
cpp_args = ['-O2']

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'cortex-a53'
endian = 'little'
X
export PKG_CONFIG_SYSROOT_DIR=/sysroot

rm -rf /b && cp -a /w/src /b && cd /b
meson setup output --cross-file /cross.txt \
	--prefix=/usr --libdir=lib --libexecdir=libexec --sysconfdir=/etc --buildtype=plain \
	-Dpipelines=simple,uvcvideo,virtual \
	-Dipas=simple \
	-Dv4l2=true \
	-Dgstreamer=enabled \
	-Dcam=enabled \
	-Dqcam=disabled \
	-Dlc-compliance=disabled \
	-Dpycamera=disabled \
	-Ddocumentation=disabled \
	-Dtest=false \
	-Dwerror=false | tee /w/out-setup.log
meson compile -C output
rm -rf /w/out && mkdir -p /w/out
DESTDIR=/w/out/dest meson install --no-rebuild -C output >/w/out/install.log
sed -n '/^libcamera /,$p' /w/out-setup.log > /w/out/meson-summary.txt
mv /w/out-setup.log /w/out/

# strip, then re-sign the IPA (signature covers the stripped file), as the pmOS APKBUILD does
find /w/out/dest -type f | while read -r f; do
	if [ "$(head -c 4 "$f" | od -An -c | tr -d ' ')" = '177ELF' ]; then llvm-strip --strip-unneeded "$f"; fi
done
for ipa in /w/out/dest/usr/lib/libcamera/ipa/ipa*.so; do
	src/ipa/ipa-sign.sh "$(find output -type f -iname '*ipa-priv-key.pem')" "$ipa" "$ipa.sign"
done
# only the files install.sh replaces, plus the tuning files
sh /w/collect.sh /w/out/dest /w/out/root && rm -rf /w/out/dest
chown -R "$(stat -c %u:%g /w)" /w/out
find /w/out/root -type f | sort
