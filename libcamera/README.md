# libcamera/: patches, tuning files and tools

[libcamera](https://libcamera.org) is the camera library that apps (Snapshot, via
PipeWire) use. The kernel only delivers raw sensor data; libcamera's **simple
pipeline** with **SoftISP** turns it into a picture on the GPU (colours, white
balance, brightness) and, with the patches here, also focuses the rear lens.

`install.sh --camera` replaces the libcamera library files of Nura with a build of
libcamera 0.7.1 plus the 14 patches below (`../prebuilt/libcamera/`), and adds the two
tuning files.

## Files

| Path | What it is |
|---|---|
| `patches/0010`–`0014` | Our patches (table below). 0001–0009 are third-party and are downloaded by `../fetch-upstream.sh`, not stored here. The series is applied on top of libcamera v0.7.1 |
| `imx386.yaml` | Tuning for the rear camera: black level, automatic white balance, colour matrix (from Xiaomi's Android tuning data), brightness/contrast, autofocus range and speed, exposure control |
| `s5k5e8.yaml` | Tuning for the front camera: black level, white balance, exposure. No colour matrix yet, so colours are less accurate than the rear camera |
| `build-on-phone.sh` | Builds the patched libcamera on the phone for the version it has (`install.sh --newversion` runs it): Nura's own package source and patches for that version, checked against Nura's checksums, plus ours. Falls back to fewer patches when ours do not fit (see below) |
| `collect.sh` | Picks the files `install.sh` replaces out of a build, and adds the tuning files |
| `prepare-src.sh` | PC (Docker): downloads libcamera and applies the patches into `src/` |
| `xbuild.sh` | PC (Docker): cross-compiles `src/` for the phone (aarch64) into `out/root/`, strips and signs the IPA module like the Nura package does |
| `test-native.sh` | Builds on the PC and runs the autofocus unit tests (added by patches 0005–0009 and 0012) |
| `tools/` | Helpers used while tuning, see below |

## Patch series

| # | What it does | Author |
|---|---|---|
| 0001–0003 | The patches Nura/postmarketOS v26.06 applies to its own libcamera package: SoftISP for the PinePhone, skip hardware-ISP formats when SoftISP is used, GPU (EGL) import of camera buffers. Not specific to the Mi Max 2; needed so the result behaves like the Nura package. Not stored here: fetched from [pmaports](https://gitlab.postmarketos.org/postmarketOS/pmaports/-/tree/v26.06/temp/libcamera) | Robert Mader |
| 0004–0009 | One-shot contrast autofocus for the simple pipeline: connect the lens to the pipeline, measure sharpness, the AF algorithm, report the AF state, and two guards against crashes when the camera stops. From [postmarketos-xiaomi-vince](https://github.com/kotXio/postmarketos-xiaomi-vince) (Redmi 5 Plus, same platform and lens chip), unmodified and not stored here (fetched from his repository, pinned): [patch series](https://github.com/kotXio/postmarketos-xiaomi-vince/tree/8632bfaa7f0dfdb35e720bb736beda19aafdfb46/patches/libcamera), [write-up](https://github.com/kotXio/postmarketos-xiaomi-vince/blob/main/fixes/ov12a10-autofocus.md) | Kostiantyn Andriiuk |
| 0010 | Sony IMX386 support: how its gain register maps to real gain (512/(512−code)), its black level and pixel size. Without this, exposure control is wrong and images too dark or too bright | Dimitris |
| 0011 | `autoTrigger` tuning option: start one focus scan per camera session on its own. Snapshot (through PipeWire) cannot ask for autofocus, so without this the lens would never move | Dimitris |
| 0012 | Unit test for 0011 | Dimitris |
| 0013 | Exposure control reacts in bigger steps when the picture is far too dark or bright (1.1x to 2x instead of a fixed 10%), so the image settles in about a second instead of several | Dimitris |
| 0014 | Samsung S5K5E8 support (gain = code/32, black level, pixel size), like 0010 for the front camera | Dimitris |

## Newer libcamera versions

`build-on-phone.sh` starts from exactly what Nura ships for the installed version and adds
our patches. The autofocus patches change the most code, so they are the first to stop
fitting a new libcamera. The build then falls back instead of failing:

| Level | What is built | Result |
|---|---|---|
| full | all patches | both cameras, autofocus, as on Nura v26.06 |
| sensor | 0010 + 0014 (+ 0013 if it fits), without autofocus; `imx386.yaml` without its `Af` block | both cameras with correct exposure, fixed focus |
| none | nothing, Nura's libcamera stays | only the tuning files (without `Af`) are installed; cameras work with Nura's libcamera |

To bring autofocus to a newer libcamera, the patches 0004–0009 and 0011 need to be
updated for it (they come from postmarketos-xiaomi-vince, which targets 0.7.1).

## Why the whole library is replaced

The autofocus lives in two places: the pipeline inside `libcamera.so` (it drives the
lens) and the IPA module `ipa_soft_simple.so` (it decides where to focus). The IPA
module is signed with a key built into `libcamera.so`, so both must come from the
same build: each build makes a new key. Otherwise libcamera runs the IPA in an
isolated process.

## Tools

| Tool | Where | Use |
|---|---|---|
| `tools/sweep.sh` | phone | Moves the lens through given positions and saves a raw crop at each |
| `tools/af_sharpness.py` | PC | Scores those crops and prints the sharpest lens position, to check the `infinity`/`macro` range in `imx386.yaml` |
| `tools/grab.sh` | phone | Grabs one raw frame with a fixed exposure and gain, without libcamera |
| `tools/raw2png.py` | PC | Turns a raw frame into a PNG to look at |

See "Tuning autofocus" in the main README for how to use them.
