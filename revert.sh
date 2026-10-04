#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Undo install.sh (on the phone):
#   sudo sh revert.sh            remove everything, restore the original DTB and libcamera
#   sudo sh revert.sh --camera   remove only the camera part
#   sudo sh revert.sh --led      remove only the LED torch part
#   sudo sh revert.sh --sim      remove only the SIM wait fix
# Then reboot. If the kernel or libcamera was upgraded since the install, the upgrade
# already put the original files back, so only our leftovers are removed.
set -eu
KERNEL_PKGNAME=linux-postmarketos-qcom-msm8953
CAMERA_MODULES="imx386 s5k5e8 dw9768 qcom-camss"
LED_MODULES="leds-qcom-pmi8950-torch"
STATE=/var/lib/mi-max-2-mainline
BACKUP=$STATE/backup
UDEV_RULE=/etc/udev/rules.d/73-oxygen-flash-not-notification.rules
DTB_NAME=msm8953-xiaomi-oxygen.dtb
SIM_DROPIN=/etc/systemd/system/msm-modem-uim-selection.service.d/sim-wait.conf
installed() { for p in camera led sim; do if [ -e "$STATE/$p" ]; then printf '%s ' "$p"; fi; done; }
pkgver() { apk info -v 2>/dev/null | grep -m1 "^$1-[0-9]" || true; }

rm_camera=0; rm_led=0; rm_sim=0
for a in "$@"; do
	case "$a" in
	--camera) rm_camera=1 ;;
	--led) rm_led=1 ;;
	--sim) rm_sim=1 ;;
	*) awk 'NR > 2 && !/^#/ { exit } NR > 2 { sub(/^# ?/, ""); print }' "$0"; exit 1 ;;
	esac
done
[ $rm_camera = 0 ] && [ $rm_led = 0 ] && [ $rm_sim = 0 ] && { rm_camera=1; rm_led=1; rm_sim=1; }

[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }
[ -d "$BACKUP" ] || { echo "nothing installed (no $BACKUP); for a full reset use: apk fix libcamera libcamera-ipa libcamera-tools $KERNEL_PKGNAME" >&2; exit 1; }

restore() {  # restore <file>: put the backed-up original back, or remove a file that did not exist
	if [ -e "$BACKUP$1" ]; then
		cp -a "$BACKUP$1" "$1.new" && mv -f "$1.new" "$1"
	else
		case "$1" in
		/boot/*) echo "WARNING: no backup of $1, leaving it as it is" >&2 ;;	# never delete a DTB
		*) apk info -q --who-owns "$1" >/dev/null 2>&1 || rm -f "$1" ;;	# never delete a package file
		esac
	fi
}

# The kernel files are only restored for the kernel they were installed on.
same_kernel=0
[ -e "$STATE/kernel-pkg" ] && [ "$(cat "$STATE/kernel-pkg")" = "$(pkgver $KERNEL_PKGNAME)" ] && same_kernel=1
krel=$(cat "$STATE/kernel-release" 2>/dev/null || true)
MODDIR=/lib/modules/$krel/updates

# Only rewrite the DTB if this revert removes a part that changed it.
dtb_parts_before=0
[ -e "$STATE/camera" ] && dtb_parts_before=$((dtb_parts_before + 1))
[ -e "$STATE/led" ] && dtb_parts_before=$((dtb_parts_before + 1))

if [ $rm_camera = 1 ] && [ -e "$STATE/camera" ]; then
	echo "== removing camera"
	for m in $CAMERA_MODULES; do rm -f "$MODDIR/$m.ko"; done
	if [ "$(cat "$STATE/libcamera-pkg")" = "$(pkgver libcamera)" ]; then
		while read -r f; do restore "$f"; done < "$STATE/libcamera-files"
	else
		echo "libcamera was upgraded since the install: keeping its files, removing only ours"
		while read -r f; do [ -e "$BACKUP$f" ] || restore "$f"; done < "$STATE/libcamera-files"
	fi
	while read -r f; do rm -f "$BACKUP$f"; done < "$STATE/libcamera-files"
	rm -f "$STATE/camera" "$STATE/libcamera-files" "$STATE/libcamera-pkg"
fi
if [ $rm_led = 1 ] && [ -e "$STATE/led" ]; then
	echo "== removing LED torch"
	for m in $LED_MODULES; do rm -f "$MODDIR/$m.ko"; done
	rm -f "$UDEV_RULE"
	udevadm control --reload 2>/dev/null || true
	rm -f "$STATE/led"
fi
if [ $rm_sim = 1 ] && [ -e "$STATE/sim" ]; then
	echo "== removing SIM wait fix"
	restore "$SIM_DROPIN"
	rm -f "$BACKUP$SIM_DROPIN"
	rmdir "$(dirname "$SIM_DROPIN")" 2>/dev/null || true
	systemctl daemon-reload
	rm -f "$STATE/sim"
fi

dtb_parts_after=0
[ -e "$STATE/camera" ] && dtb_parts_after=$((dtb_parts_after + 1))
[ -e "$STATE/led" ] && dtb_parts_after=$((dtb_parts_after + 1))
if [ $dtb_parts_after != $dtb_parts_before ]; then
	[ -d "/lib/modules/$krel" ] && depmod -a "$krel"
	rmdir "$MODDIR" 2>/dev/null || true

	# DTB for whatever is still installed, or the original one.
	if [ -e "$STATE/camera" ] && [ -e "$STATE/led" ]; then dtb=camera-led
	elif [ -e "$STATE/camera" ]; then dtb=camera
	elif [ -e "$STATE/led" ]; then dtb=led
	else dtb=""
	fi
	if [ $same_kernel = 1 ]; then
		for f in /boot/$DTB_NAME /boot/dtbs/qcom/$DTB_NAME; do
			[ -d "$(dirname "$f")" ] || continue
			if [ -n "$dtb" ]; then
				install -m644 "$STATE/dtb/$dtb.dtb" "$f.new" && mv -f "$f.new" "$f"
			else
				restore "$f"
			fi
		done
	else
		echo "the kernel was upgraded since the install: its package already put the original DTB back"
	fi
	if [ -z "$dtb" ]; then
		rm -rf "${STATE:?}/dtb" "${BACKUP:?}/boot" "$STATE/kernel-pkg" "$STATE/kernel-release"
	fi
fi
[ -n "$(installed)" ] || rm -rf "${STATE:?}"
sync
echo "Reverted. Still installed: $(installed)"
echo "Reboot now:  sudo reboot"
