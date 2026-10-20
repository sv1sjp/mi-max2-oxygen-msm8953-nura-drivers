#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Undo install.sh (on the phone):
#   sudo sh revert.sh            remove everything, restore the original DTB and libcamera
#   sudo sh revert.sh --camera   remove only the camera part
#   sudo sh revert.sh --led      remove only the LED torch part
#   sudo sh revert.sh --speaker  remove only the loudspeaker + earpiece part
#   sudo sh revert.sh --sim      remove only the SIM wait fix
#   sudo sh revert.sh --gpu      remove only the GTK GPU renderer setting and the touch boost
#   sudo sh revert.sh --video    remove only the hardware video decoder fix
# Then reboot. If the kernel or libcamera was upgraded since the install, the upgrade
# already put the original files back, so only our leftovers are removed.
set -eu
KERNEL_PKGNAME=linux-postmarketos-qcom-msm8953
CAMERA_MODULES="imx386 s5k5e8 dw9768 qcom-camss"
LED_MODULES="leds-qcom-pmi8950-torch leds-aw2013"
SPEAKER_MODULES="tas2560 fastrpc"
VIDEO_MODULES="venus-core venus-dec"
VIDEO_MODPROBE=/etc/modprobe.d/oxygen-venus.conf
HRPC_RULE=/etc/udev/rules.d/74-oxygen-hexagonrpcd-adsp.rules
UCM_FILES="/usr/share/alsa/ucm2/Xiaomi/oxygen/HiFi.conf /usr/share/alsa/ucm2/conf.d/xiaomi-oxygen/xiaomi-oxygen.conf"
STATE=/var/lib/mi-max-2-mainline
BACKUP=$STATE/backup
UDEV_RULE=/etc/udev/rules.d/73-oxygen-flash-not-notification.rules
DTB_NAME=msm8953-xiaomi-oxygen.dtb
SIM_DROPIN=/etc/systemd/system/msm-modem-uim-selection.service.d/sim-wait.conf
GPU_PROFILE=/etc/profile.d/zz-oxygen-gtk-gpu-renderer.sh
BOOST_BIN=/usr/libexec/oxygen-touchboost
BOOST_UNIT=oxygen-touchboost.service
installed() { for p in camera led speaker sim gpu video; do if [ -e "$STATE/$p" ]; then printf '%s ' "$p"; fi; done; }
dtb_variant() { v=""; for p in camera led speaker; do if [ -e "$STATE/$p" ]; then v="${v:+$v-}$p"; fi; done; echo "$v"; }
pkgver() { apk info -v 2>/dev/null | grep -m1 "^$1-[0-9]" || true; }

rm_camera=0; rm_led=0; rm_speaker=0; rm_sim=0; rm_gpu=0; rm_video=0
for a in "$@"; do
	case "$a" in
	--camera) rm_camera=1 ;;
	--led) rm_led=1 ;;
	--speaker) rm_speaker=1 ;;
	--sim) rm_sim=1 ;;
	--gpu) rm_gpu=1 ;;
	--video) rm_video=1 ;;
	*) awk 'NR > 2 && !/^#/ { exit } NR > 2 { sub(/^# ?/, ""); print }' "$0"; exit 1 ;;
	esac
done
[ $rm_camera = 0 ] && [ $rm_led = 0 ] && [ $rm_speaker = 0 ] && [ $rm_sim = 0 ] && [ $rm_gpu = 0 ] \
	&& [ $rm_video = 0 ] && { rm_camera=1; rm_led=1; rm_speaker=1; rm_sim=1; rm_gpu=1; rm_video=1; }

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
dtb_before=$(dtb_variant)

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
if [ $rm_speaker = 1 ] && [ -e "$STATE/speaker" ]; then
	echo "== removing loudspeaker"
	for m in $SPEAKER_MODULES; do rm -f "$MODDIR/$m.ko"; done
	rm -f "$HRPC_RULE"
	udevadm control --reload 2>/dev/null || true
	for f in $UCM_FILES; do restore "$f"; rm -f "$BACKUP$f"; done
	rm -f "$STATE/speaker"
fi
if [ $rm_sim = 1 ] && [ -e "$STATE/sim" ]; then
	echo "== removing SIM wait fix"
	restore "$SIM_DROPIN"
	rm -f "$BACKUP$SIM_DROPIN"
	rmdir "$(dirname "$SIM_DROPIN")" 2>/dev/null || true
	systemctl daemon-reload
	rm -f "$STATE/sim"
fi
video_removed=0
if [ $rm_video = 1 ] && [ -e "$STATE/video" ]; then
	echo "== removing hardware video decoder fix"
	for m in $VIDEO_MODULES; do rm -f "$MODDIR/$m.ko"; done
	rm -f "$VIDEO_MODPROBE"
	rm -f "$STATE/video"
	video_removed=1
fi
if [ $rm_gpu = 1 ] && [ -e "$STATE/gpu" ]; then
	echo "== removing GTK GPU renderer setting and touch boost"
	rm -f "$GPU_PROFILE"
	systemctl disable --now "$BOOST_UNIT" >/dev/null 2>&1 || true	# restores the clocks
	rm -f "$BOOST_BIN" "/etc/systemd/system/$BOOST_UNIT"
	systemctl daemon-reload
	rm -f "$STATE/gpu"
fi

dtb=$(dtb_variant)
if [ $video_removed = 1 ] && [ "$dtb" = "$dtb_before" ] && [ -d "/lib/modules/$krel" ]; then
	depmod -a "$krel"	# the video part has no DTB, so the block below may not run
	rmdir "$MODDIR" 2>/dev/null || true
fi
if [ "$dtb" != "$dtb_before" ]; then
	[ -d "/lib/modules/$krel" ] && depmod -a "$krel"
	rmdir "$MODDIR" 2>/dev/null || true

	# DTB for whatever is still installed ($dtb), or the original one.
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
if [ $rm_camera = 0 ] && [ $rm_led = 0 ] && [ $rm_speaker = 0 ] && [ $rm_sim = 0 ] && [ $rm_video = 0 ]; then
	echo "Log out and in again (or reboot) for the GTK renderer change."
else
	echo "Reboot now:  sudo reboot"
fi
