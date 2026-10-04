#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Write beeps-1.wav .. beeps-4.wav (N beeps of 660 Hz, 48 kHz stereo S16), the test sounds
# the *-test.sh scripts play, so a listener can tell which variant made a sound.
import math, struct, wave

RATE, FREQ, AMP = 48000, 660, 8191
BEEP, PERIOD, TAIL, FADE = 0.33, 0.58, 1.75, 0.01

for n in range(1, 5):
    frames = bytearray()
    for b in range(n):
        for i in range(int(BEEP * RATE)):
            t = i / RATE
            env = min(1.0, t / FADE, (BEEP - t) / FADE)
            v = round(AMP * env * math.sin(2 * math.pi * FREQ * t))
            frames += struct.pack('<hh', v, v)
        if b < n - 1:
            frames += bytes(4 * int((PERIOD - BEEP) * RATE))
    frames += bytes(4 * int(TAIL * RATE))
    with wave.open(f'beeps-{n}.wav', 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))
