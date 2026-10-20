#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Touch boost for the Xiaomi Mi Max 2 (oxygen), like Android's input boost.

While a finger is on the touchscreen, and for BOOST_S seconds after the last touch
event, raise the minimum CPU and GPU clocks so the first frames of a scroll or
swipe are not drawn at idle clocks. Afterwards the minimums go back to the values
they had when this service started. Installed by install.sh --gpu.
"""
import os
import select
import signal
import struct
import sys
import time

CPU_POLICY = "/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq"
GPU_MIN = "/sys/class/devfreq/1c00000.gpu/min_freq"
CPU_BOOST = int(os.environ.get("CPU_BOOST_KHZ", "1420800"))
GPU_BOOST = int(os.environ.get("GPU_BOOST_HZ", "400000000"))
BOOST_S = float(os.environ.get("BOOST_S", "1.0"))
ABS_MT_POSITION_X = 0x35
EVENT_SIZE = struct.calcsize("llHHi")


def read(path):
    with open(path) as f:
        return f.read().strip()


def write(path, value):
    try:
        with open(path, "w") as f:
            f.write(str(value))
    except OSError as e:
        print(f"touchboost: cannot write {value} to {path}: {e}", file=sys.stderr)


def touchscreens():
    """Event devices that report multi-touch positions."""
    found = []
    for name in sorted(os.listdir("/sys/class/input")):
        if not name.startswith("event"):
            continue
        try:
            bits = read(f"/sys/class/input/{name}/device/capabilities/abs").split()
        except OSError:
            continue
        mask = int(bits[-1], 16) if bits else 0	# the lowest 64 ABS codes are in the last word
        if mask >> ABS_MT_POSITION_X & 1:
            found.append(f"/dev/input/{name}")
    return found


def main():
    rest = {CPU_POLICY: read(CPU_POLICY), GPU_MIN: read(GPU_MIN)}
    boost = {CPU_POLICY: CPU_BOOST, GPU_MIN: GPU_BOOST}

    def restore(*_):
        for path, value in rest.items():
            write(path, value)
        sys.exit(0)

    signal.signal(signal.SIGTERM, restore)
    signal.signal(signal.SIGINT, restore)

    devices = {}
    boosted_until = 0.0
    while True:
        if not devices:
            if boosted_until:
                for path, value in rest.items():
                    write(path, value)
                boosted_until = 0.0
            for path in touchscreens():
                try:
                    devices[os.open(path, os.O_RDONLY | os.O_NONBLOCK)] = path
                except OSError:
                    pass
            if not devices:
                time.sleep(5)	# no touchscreen (yet): look again later
                continue
            print("touchboost: watching " + " ".join(devices.values()), flush=True)
        now = time.monotonic()
        timeout = max(0.0, boosted_until - now) if boosted_until else None
        ready, _, _ = select.select(list(devices), [], [], timeout)
        for fd in ready:
            try:
                data = os.read(fd, EVENT_SIZE * 64)
            except OSError:
                data = b""
            if not data:	# device gone (driver rebound): find it again
                os.close(fd)
                del devices[fd]
                continue
            if not boosted_until:
                for path, value in boost.items():
                    write(path, value)
            boosted_until = time.monotonic() + BOOST_S
        if boosted_until and time.monotonic() >= boosted_until:
            for path, value in rest.items():
                write(path, value)
            boosted_until = 0.0


if __name__ == "__main__":
    main()
