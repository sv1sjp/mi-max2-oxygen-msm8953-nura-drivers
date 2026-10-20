<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# GPU renderer for GTK4 apps

`install.sh --gpu` installs [`zz-oxygen-gtk-gpu-renderer.sh`](zz-oxygen-gtk-gpu-renderer.sh)
as `/etc/profile.d/zz-oxygen-gtk-gpu-renderer.sh`. It sets `GSK_RENDERER=ngl`, so GTK4 apps
(Snapshot, Settings, Files, Console, Calls, ...) draw on the GPU instead of the CPU.
Phosh and phoc are GTK3 and do not use this variable.

## Why

Nura's `soc-qcom-msm8953-gpu` package ships `/etc/profile.d/adreno-a506-quirks.sh` with
`GSK_RENDERER=cairo`, the software renderer. It was added when GTK's new ngl renderer was very
slow on the Adreno 506 ([GTK issue 6576](https://gitlab.gnome.org/GNOME/gtk/-/issues/6576)) and
the old `gl` renderer, the earlier workaround, was being removed from GTK.

With GTK 4.22 and Mesa 26.1 (freedreno) ngl is fast on this GPU. Measured on the phone
(2026-10-18) with the Snapshot camera preview at 1920x1080:

| renderer | Snapshot CPU use | preview |
|---|---|---|
| cairo (Nura default) | ~1.5 cores | delayed, choppy |
| ngl (this file) | ~0.25 cores | smoother, no glitches |

Settings, Files, Console and Calculator were also checked by hand: smoother or the same,
no rendering errors.

Our file sorts after `adreno-a506-quirks.sh` in `/etc/profile.d`, so it wins and the Nura
package stays untouched. The change takes effect at the next login (or reboot).

## Touch boost

`--gpu` also installs `oxygen-touchboost` ([`oxygen-touchboost.py`](oxygen-touchboost.py) as
`/usr/libexec/oxygen-touchboost`, run by [`oxygen-touchboost.service`](oxygen-touchboost.service)).
While a finger is on the screen, and for 1 s after the last touch event, it raises the minimum
CPU clock to 1.42 GHz and the minimum GPU clock to 400 MHz; then it puts back the minimums it
found at start (19.2 MHz GPU, 652.8 MHz CPU).

Why: comparing the drivers with Android's (the downstream oxygen device tree and Qualcomm's
msm8953 clock driver) showed that mainline already has the same maximum GPU clock (650 MHz;
Android's KGSL table also lists 725 MHz, but on msm8953 its clock driver stops at 650), the same
GPU memory-bandwidth steps (up to 7.45 GB/s) and a normal display bandwidth vote. The difference
is how fast the clocks react. Android boosts the CPU to 1.4 GHz on every touch (`cpu_boost`
input boost) and KGSL jumps the GPU straight to 400 MHz when it gets busy (`ca-jump`, target
power level 4). Mainline's GPU governor samples every 50 ms, and its wake-up boost doubles the
current clock, which after a pause is 19.2 MHz → 38.4 MHz. So the first 50–100 ms of each scroll
were drawn too slowly. With the boost the user found scrolling "a little better".

Measured scrolling in Settings without boost: mostly 40–59 fps, with short drops to 0–7 fps.

Settings (environment variables in the service, if you want to tune them): `CPU_BOOST_KHZ`
(default 1420800), `GPU_BOOST_HZ` (400000000), `BOOST_S` (1.0).

## Try it without installing

```sh
systemctl --user set-environment GSK_RENDERER=ngl
dbus-update-activation-environment --systemd GSK_RENDERER=ngl
```

Apps started afterwards use ngl; `GSK_RENDERER=cairo` in the same commands goes back.
To try a single app: `GSK_RENDERER=ngl snapshot`.

## Undo

`sudo sh revert.sh --gpu`, then log out and in (or reboot). This also stops the touch boost and
puts the original minimum clocks back.
