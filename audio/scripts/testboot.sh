#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Run a test on a RAM-booted image, then ALWAYS return the phone to the normal OS.
# usage: testboot.sh <image.img> '<commands to run on the phone as user>'
# Needs: fastboot, the phone booted normally and reachable over SSH without a password
# prompt (ssh-copy-id user@172.16.42.1), PHONE_PASS set to the phone password (for sudo).
# PHONE overrides the SSH target (default: user@172.16.42.1, USB networking).
set -u
IMG=$1; CMDS=$2
: "${PHONE_PASS:?set PHONE_PASS to the phone password (for sudo on the phone)}"
P="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o BatchMode=yes ${PHONE:-user@172.16.42.1}"
timeout 30 $P "echo '$PHONE_PASS' | sudo -S -p '' systemctl reboot --reboot-argument=bootloader"
for _ in $(seq 1 40); do timeout 3 fastboot devices | grep -q fastboot && break; sleep 3; done
timeout 120 fastboot boot "$IMG"
sleep 45; for _ in $(seq 1 40); do timeout 8 $P true 2>/dev/null && break; sleep 5; done
timeout 600 $P "grep -o loglevel=7 /proc/cmdline; for i in \$(seq 1 30); do grep -q running /sys/class/remoteproc/remoteproc2/state && break; sleep 3; done; sleep 15; $CMDS"
echo "=== RESTORE"
timeout 20 $P "echo '$PHONE_PASS' | sudo -S -p '' systemctl reboot" >/dev/null 2>&1; sleep 45
for _ in $(seq 1 50); do timeout 8 $P 'grep -q "^quiet splash" /proc/cmdline && pgrep -x phoc >/dev/null' 2>/dev/null && break; sleep 5; done
timeout 20 $P 'cut -c1-12 /proc/cmdline; ls /dev/dri | tr "\n" " "; pgrep -a phrog || pgrep -a phosh' || echo "!!! NOT BACK IN NORMAL OS - reboot again"
