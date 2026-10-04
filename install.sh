#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Xiaomi Mi Max 2 (oxygen): cameras, rear LED torch and SIM fix for Nura (formerly postmarketOS)
#
#   sudo sh install.sh               install everything (camera + LED + SIM)
#   sudo sh install.sh --camera      cameras only: rear imx386 + dw9768(dw9763) AF, front s5k5e8,
#                                    fixed qcom-camss, patched libcamera (AF, gain models, AGC)
#   sudo sh install.sh --led         rear LED torch only: leds-qcom-pmi8950-torch + udev rule
#   sudo sh install.sh --sim         SIM card only: longer SIM wait for msm-modem-uim-selection
#   sudo sh install.sh --check       only run the checks, change nothing (combine with the above)
#   sudo sh install.sh --ignore      install the prebuilt files although the kernel or libcamera
#                                    package revision differs (same kernel 7.0.9-msm8953 and
#                                    libcamera 0.7.1 inside, so it most likely works)
#   sudo sh install.sh --newversion  for another kernel or libcamera version (newer Nura):
#                                    builds what is needed on the phone first, for exactly
#                                    the installed versions (needs internet, up to ~1.5 hours)
#
# Undo: sudo sh revert.sh [--camera] [--led] [--sim]. A reboot is needed afterwards.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
# The versions prebuilt/ was made for (Nura v26.06)
KVER=7.0.9-msm8953
KERNEL_PKGNAME=linux-postmarketos-qcom-msm8953
KERNEL_PKG=$KERNEL_PKGNAME-7.0.9-r0
LIBCAMERA_PKG=libcamera-99990.7.1-r0
LIBCAMERA_SO=libcamera.so.0.7.1
CAMERA_MODULES="imx386 s5k5e8 dw9768 qcom-camss"
LED_MODULES="leds-qcom-pmi8950-torch"
STATE=/var/lib/mi-max-2-mainline
BACKUP=$STATE/backup
UDEV_RULE=/etc/udev/rules.d/73-oxygen-flash-not-notification.rules
DTB_NAME=msm8953-xiaomi-oxygen.dtb
SIM_UNIT=msm-modem-uim-selection.service
SIM_DROPIN=/etc/systemd/system/$SIM_UNIT.d/sim-wait.conf
installed() { for p in camera led sim; do if [ -e "$STATE/$p" ]; then printf '%s ' "$p"; fi; done; }
pkgver() { apk info -v 2>/dev/null | grep -m1 "^$1-[0-9]" || true; }

die() { echo "ERROR: $*" >&2; exit 1; }
usage() { awk 'NR > 2 && !/^#/ { exit } NR > 2 { sub(/^# ?/, ""); print }' "$0"; exit "${1:-0}"; }

want_camera=0; want_led=0; want_sim=0; check_only=0; newversion=0; ignore=0
for a in "$@"; do
	case "$a" in
	--camera) want_camera=1 ;;
	--led) want_led=1 ;;
	--sim) want_sim=1 ;;
	--check) check_only=1 ;;
	--newversion) newversion=1 ;;
	--ignore) ignore=1 ;;
	-h|--help) usage ;;
	*) echo "unknown option: $a" >&2; usage 1 ;;
	esac
done
[ $want_camera = 0 ] && [ $want_led = 0 ] && [ $want_sim = 0 ] && { want_camera=1; want_led=1; want_sim=1; }
want_kernel=0; { [ $want_camera = 1 ] || [ $want_led = 1 ]; } && want_kernel=1

[ "$(id -u)" = 0 ] || die "run as root (sudo sh install.sh)"
tr '\0' '\n' < /proc/device-tree/compatible | grep -qx 'xiaomi,oxygen' \
	|| die "this is not a Xiaomi Mi Max 2 (oxygen)"

# Where the kernel and libcamera files come from:
#  - prebuilt/ when the installed packages are exactly the ones it was made for;
#  - prebuilt/ with --ignore when only the package revision differs (same kernel
#    release / same libcamera library version, so the files still fit);
#  - with --newversion, a build for exactly the installed versions: made on the phone by
#    kernel/build-modules.sh --phone and libcamera/build-on-phone.sh (or on a PC, see
#    README.md) into kernel/out/<release>/ and libcamera/out/root/.
# A different kernel version cannot use prebuilt/ at all: Linux refuses modules built
# for another kernel, and a device tree for another kernel may not boot.
build_hint="run install.sh --newversion instead: it builds them on the phone for your versions"
warn() { echo "WARNING: $*" >&2; }
kbuild=0; lbuild=0
if [ $want_kernel = 1 ]; then
	krel=$(uname -r); kpkg=$(pkgver $KERNEL_PKGNAME)
	[ -d "/lib/modules/$krel/kernel" ] || die "kernel $krel is no longer installed (kernel upgraded?); reboot first"
	if [ "$kpkg" = "$KERNEL_PKG" ] && [ "$krel" = "$KVER" ]; then
		KSRC=$HERE/prebuilt/kernel
	elif [ $newversion = 1 ]; then
		KSRC=$HERE/kernel/out/$krel
		# reuse an earlier build only if it was made for this very kernel package
		[ "$(cat "$KSRC/package" 2>/dev/null)" = "$kpkg" ] || kbuild=1
	elif [ "$krel" = "$KVER" ] && [ $ignore = 1 ]; then
		warn "kernel package is ${kpkg:-unknown}, not $KERNEL_PKG; installing the prebuilt files anyway (--ignore)"
		KSRC=$HERE/prebuilt/kernel
	elif [ "$krel" = "$KVER" ]; then
		die "kernel package is ${kpkg:-unknown}, the prebuilt files were made for $KERNEL_PKG. The kernel version is the same ($KVER), so they will most likely work: add --ignore to install them anyway, or $build_hint"
	else
		die "kernel is $krel, the prebuilt modules are for $KVER. Linux refuses to load modules built for another kernel version, so --ignore cannot help here; $build_hint"
	fi
fi
if [ $want_sim = 1 ]; then
	systemctl cat "$SIM_UNIT" >/dev/null 2>&1 \
		|| die "$SIM_UNIT not found (the SIM fix needs Nura/postmarketOS with systemd and msm-modem-uim-selection)"
fi
if [ $want_camera = 1 ]; then
	lpkg=$(pkgver libcamera)
	[ -n "$lpkg" ] || die "libcamera is not installed (apk add libcamera libcamera-ipa)"
	same_lib=0; apk info -q --who-owns "/usr/lib/$LIBCAMERA_SO" >/dev/null 2>&1 && same_lib=1
	if [ "$lpkg" = "$LIBCAMERA_PKG" ]; then
		LSRC=$HERE/prebuilt/libcamera
	elif [ $newversion = 1 ]; then
		LSRC=$HERE/libcamera/out/root
		[ "$(cat "$HERE/libcamera/out/package" 2>/dev/null)" = "$lpkg" ] || lbuild=1
	elif [ $same_lib = 1 ] && [ $ignore = 1 ]; then
		warn "libcamera package is $lpkg, not $LIBCAMERA_PKG; installing the prebuilt files anyway (--ignore)"
		LSRC=$HERE/prebuilt/libcamera
	elif [ $same_lib = 1 ]; then
		die "libcamera package is $lpkg, the prebuilt files were made for $LIBCAMERA_PKG. The library version is the same ($LIBCAMERA_SO), so they will most likely work: add --ignore to install them anyway, or $build_hint"
	else
		die "libcamera is $lpkg, the prebuilt files are $LIBCAMERA_SO. Mixed with another libcamera version they would break the camera, so --ignore cannot help here; $build_hint"
	fi
fi

echo "== verifying files"
(cd "$HERE" && sha256sum -c SHA256SUMS >/dev/null) || die "checksum mismatch, the folder is incomplete or modified"

parts=""; [ $want_camera = 1 ] && parts="camera"; [ $want_led = 1 ] && parts="${parts:+$parts }led"
[ $want_sim = 1 ] && parts="${parts:+$parts }sim"
if [ $check_only = 1 ]; then
	[ $kbuild = 1 ] && echo "Would build the kernel modules for $krel ($kpkg) on the phone first."
	[ $lbuild = 1 ] && echo "Would build libcamera for $lpkg on the phone first."
	echo "Checks passed for: $parts. Nothing was changed."
	exit 0
fi

# --newversion: build what is missing, on the phone. Keep it from suspending meanwhile.
nosleep() {
	if command -v systemd-inhibit >/dev/null 2>&1; then
		systemd-inhibit --what=sleep:idle --who=mi-max-2-mainline --why="building drivers" "$@"
	else
		"$@"
	fi
}
if [ $kbuild = 1 ] || [ $lbuild = 1 ]; then
	echo "== building for your versions on the phone. This needs internet and takes a while"
	echo "   (kernel modules ~20-40 min, libcamera ~30-60 min). Keep the phone charging."
	echo "   Nothing is installed before the builds succeed."
fi
if [ $kbuild = 1 ]; then
	nosleep sh "$HERE/kernel/build-modules.sh" --phone \
		|| die "building the kernel modules for $krel failed (see above); nothing was installed"
	echo "$kpkg" > "$KSRC/package"
fi
if [ $lbuild = 1 ]; then
	nosleep sh "$HERE/libcamera/build-on-phone.sh" \
		|| die "building libcamera for $lpkg failed (see above); nothing was installed"
	echo "$lpkg" > "$HERE/libcamera/out/package"
fi

# Last checks on the files that will be installed
if [ $want_kernel = 1 ]; then
	kfiles="camera.dtb led.dtb camera-led.dtb"
	[ $want_camera = 1 ] && for m in $CAMERA_MODULES; do kfiles="$kfiles $m.ko"; done
	[ $want_led = 1 ] && for m in $LED_MODULES; do kfiles="$kfiles $m.ko"; done
	for f in $kfiles; do
		[ -e "$KSRC/$f" ] || die "$KSRC/$f is missing"
		case "$f" in *.ko)
			[ "$(modinfo -F vermagic "$KSRC/$f" | cut -d' ' -f1)" = "$krel" ] \
				|| die "$KSRC/$f is not built for kernel $krel";;
		esac
	done
fi
if [ $want_camera = 1 ]; then
	# What the libcamera part brings: full = our patched libcamera with autofocus;
	# sensor = patched libcamera without autofocus (the autofocus patches did not fit this
	# libcamera version); none = Nura's libcamera stays, only our tuning files are added.
	level=full
	[ "$LSRC" = "$HERE/libcamera/out/root" ] && level=$(cat "$HERE/libcamera/out/level" 2>/dev/null || echo full)
	if [ "$level" != none ]; then
		lib=$(cd "$LSRC/usr/lib" 2>/dev/null && ls libcamera.so.*.*.* 2>/dev/null | head -n1 || true)
		[ -n "$lib" ] && [ -e "/usr/lib/$lib" ] \
			|| die "the libcamera files in $LSRC (${lib:-none}) do not match the phone's libcamera ($lpkg)"
	fi
fi

mkdir -p "$BACKUP"
backup() {  # backup <file>: keep the pre-install version once
	[ -e "$1" ] && [ ! -e "$BACKUP$1" ] || return 0
	mkdir -p "$BACKUP$(dirname "$1")" && cp -a "$1" "$BACKUP$1"
}
put() {  # put <src> <dst> <mode>: atomic replace
	mkdir -p "$(dirname "$2")"
	install -m "$3" "$1" "$2.new" && mv -f "$2.new" "$2"
}

if [ $want_kernel = 1 ]; then
	MODDIR=/lib/modules/$krel/updates
	if [ -e "$STATE/kernel-pkg" ] && [ "$(cat "$STATE/kernel-pkg")" != "$kpkg" ]; then
		# The kernel was upgraded since the last install. Its package put back the
		# original DTB, so the old backup is outdated, and our modules for the old
		# kernel are leftovers.
		echo "== kernel upgraded since the last install: forgetting the old DTB backup"
		rm -f "$BACKUP/boot/$DTB_NAME" "$BACKUP/boot/dtbs/qcom/$DTB_NAME"
		old=$(cat "$STATE/kernel-release")
		for m in $CAMERA_MODULES $LED_MODULES; do rm -f "/lib/modules/$old/updates/$m.ko"; done
		rmdir "/lib/modules/$old/updates" "/lib/modules/$old" 2>/dev/null || true
	fi
	echo "== backing up the DTB to $BACKUP"
	backup /boot/$DTB_NAME
	backup /boot/dtbs/qcom/$DTB_NAME
	echo "$kpkg" > "$STATE/kernel-pkg"
	echo "$krel" > "$STATE/kernel-release"
	# revert.sh uses these to switch to the DTB of the parts that remain
	mkdir -p "$STATE/dtb"
	for v in camera led camera-led; do put "$KSRC/$v.dtb" "$STATE/dtb/$v.dtb" 644; done
fi

if [ $want_camera = 1 ]; then
	echo "== camera: kernel modules -> $MODDIR"
	for m in $CAMERA_MODULES; do put "$KSRC/$m.ko" "$MODDIR/$m.ko" 644; done
	echo "== camera: libcamera"
	# Back up the originals only on the first camera install: on a re-install the
	# current files are ours, and files that did not exist originally (no backup)
	# must stay without one so that revert.sh removes them. After a libcamera
	# upgrade the package has replaced our files: forget the outdated backups.
	first_camera=1; : > "$STATE/ours"
	if [ -e "$STATE/camera" ]; then
		first_camera=0
		if [ "$(cat "$STATE/libcamera-pkg")" != "$lpkg" ]; then
			echo "== libcamera upgraded since the last install: forgetting the old backups"
			while read -r f; do
				if [ -e "$BACKUP$f" ]; then rm -f "$BACKUP$f"; else echo "$f" >> "$STATE/ours"; fi
			done < "$STATE/libcamera-files"
			first_camera=1
		fi
	fi
	(cd "$LSRC" && find . -type f | sed 's|^\.||' | sort) > "$STATE/libcamera-files.new"
	while read -r t; do
		if [ $first_camera = 1 ] && ! grep -qxF "$t" "$STATE/ours"; then backup "$t"; fi
		case "$t" in *.yaml|*.sign) mode=644 ;; *) mode=755 ;; esac
		put "$LSRC$t" "$t" $mode
	done < "$STATE/libcamera-files.new"
	# keep the files of an earlier install listed too, so revert.sh handles them
	[ -e "$STATE/libcamera-files" ] && cat "$STATE/libcamera-files" >> "$STATE/libcamera-files.new"
	sort -u "$STATE/libcamera-files.new" > "$STATE/libcamera-files" && rm -f "$STATE/libcamera-files.new" "$STATE/ours"
	echo "$lpkg" > "$STATE/libcamera-pkg"
	touch "$STATE/camera"
fi

if [ $want_led = 1 ]; then
	echo "== LED: kernel module -> $MODDIR"
	for m in $LED_MODULES; do put "$KSRC/$m.ko" "$MODDIR/$m.ko" 644; done
	echo "== LED: udev rule (no notification blinking on the flash LED)"
	put "$HERE/kernel/torch/73-oxygen-flash-not-notification.rules" "$UDEV_RULE" 644
	udevadm control --reload 2>/dev/null || true
	touch "$STATE/led"
fi

if [ $want_sim = 1 ]; then
	# The modem reads the SIM later than msm-modem-uim-selection's default 4 s
	# wait, so no SIM application gets selected and ModemManager reports
	# sim-missing. See "SIM card and modem" in README.md.
	echo "== SIM: $SIM_DROPIN"
	# A copy of our own file (e.g. installed by hand from README.md, possibly
	# with different comments) is not an original, so only back up anything else.
	if [ -e "$SIM_DROPIN" ] && [ "$(grep -v '^#' "$SIM_DROPIN")" != "$(grep -v '^#' "$HERE/modem/sim-wait.conf")" ]; then
		backup "$SIM_DROPIN"
	fi
	put "$HERE/modem/sim-wait.conf" "$SIM_DROPIN" 644
	systemctl daemon-reload
	touch "$STATE/sim"
fi

if [ $want_kernel = 1 ]; then
	depmod -a "$krel"
	if [ -e "$STATE/camera" ]; then
		[ "$(modinfo -k "$krel" -n qcom_camss)" = "$MODDIR/qcom-camss.ko" ] || die "depmod did not pick the fixed qcom-camss"
	fi

	# One DTB carries everything that is installed (camera, LED or both).
	if [ -e "$STATE/camera" ] && [ -e "$STATE/led" ]; then dtb=camera-led
	elif [ -e "$STATE/camera" ]; then dtb=camera
	else dtb=led
	fi
	echo "== device tree: $dtb"
	for f in /boot/$DTB_NAME /boot/dtbs/qcom/$DTB_NAME; do
		if [ -d "$(dirname "$f")" ]; then put "$KSRC/$dtb.dtb" "$f" 644; fi
	done
fi
sync

echo
echo "Done. Installed now: $(installed)(active after a reboot)"
if [ $want_camera = 1 ]; then
	case $level in
	sensor) echo "Camera: our libcamera patches without autofocus (they do not fit $lpkg yet)" ;;
	none) echo "Camera: Nura's own libcamera with our tuning files (our patches do not fit $lpkg yet)" ;;
	esac
fi
[ $kbuild = 1 ] || [ $lbuild = 1 ] && echo "Built on this phone for: ${kpkg:-} ${lpkg:-}. If it works, please report it; if not, run revert.sh."
echo "Reboot now:  sudo reboot"
echo "Notes:"
echo " - a kernel or libcamera upgrade replaces these files: run install.sh again"
echo "   (with --newversion and your own build if the version changed)"
echo " - backup of the replaced files: $BACKUP"
