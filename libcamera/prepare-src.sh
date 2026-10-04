#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Download libcamera and apply the patch series into ./src (used by xbuild.sh).
# Run from this directory:  docker run --rm -v "$PWD:/w" alpine:3.24 sh /w/prepare-src.sh
# Default: v0.7.1 (the prebuilt files). For the version your phone has (apk info -v libcamera),
# add -e VERSION=0.8.0; patches that no longer apply must then be updated by hand.
set -eu
ls /w/patches/0009-*.patch >/dev/null 2>&1 || { echo "ERROR: third-party patches missing: run sh fetch-upstream.sh (repository root) first" >&2; exit 1; }
VERSION=${VERSION:-0.7.1}
SHA512_071=0e886021a3bbd668184b581248b9d89a8e909360a7f138237a2c03b41b477cb255bd6daf4932fdebf86c3410b4deccc4c5db93264ed0c6c9969076cdbae8a77b
apk add -q git curl
cd /w
trap 'chown -R "$(stat -c %u:%g /w)" /w/src /w/libcamera-v*.tar.gz 2>/dev/null' EXIT
T=libcamera-v$VERSION.tar.gz
curl -sSfL -o $T https://gitlab.freedesktop.org/camera/libcamera/-/archive/v$VERSION/$T
if [ "$VERSION" = 0.7.1 ]; then echo "$SHA512_071  $T" | sha512sum -c -; fi
rm -rf src && mkdir src && tar xzf $T -C src --strip-components=1
cd src
G="git -c user.name=build -c user.email=build@localhost"
git init -q && git add -A && $G commit -qm v$VERSION
for p in /w/patches/*.patch; do
	$G am -q "$p" 2>/dev/null && continue
	$G am --abort
	if git apply -R --check "$p" 2>/dev/null; then echo "already in libcamera $VERSION: $(basename "$p")"
	else echo "ERROR: $(basename "$p") does not apply to libcamera $VERSION and needs updating" >&2; exit 1; fi
done
git log --oneline | head -14
