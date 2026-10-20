# kernel/: drivers, patches and device tree

Everything the Linux kernel needs for the cameras, the rear LED torch, the loudspeaker and the earpiece. None of it
replaces the Nura kernel: `install.sh` adds extra modules to
`/lib/modules/<kernel>/updates/` (the kernel prefers them over its own copies) and
swaps the device tree for one with the new hardware described.

## How the pieces fit together

```
device tree (dts/)  ── says which chips are where: sensors, lens, LED, their
       │               power rails, clocks, GPIOs and CSI lanes
       ▼
kernel drivers      ── imx386.ko, s5k5e8.ko   talk to the camera sensors over I²C (CCI)
                       dw9768.ko              moves the rear lens (autofocus)
                       qcom-camss.ko          receives the image data (CSI) into memory
                       leds-qcom-pmi8950-torch.ko   switches the rear LED
                       leds-aw2013.ko         notification LED (fixed stock driver)
                       tas2560.ko             loudspeaker and earpiece amplifiers (I²C control, audio via the ADSP)
                       fastrpc.ko             FastRPC to the ADSP (unchanged upstream driver)
                       venus-core.ko, venus-dec.ko   hardware video decoder (fixed stock driver)
       ▼
/dev/video*, /dev/v4l-subdev*, /sys/class/leds/white:flash, the sound card, /dev/fastrpc-adsp
       ▼
libcamera (../libcamera/) turns raw sensor data into a picture; Phosh uses the LED;
PulseAudio plays through the Speaker / Earpiece ports; hexagonrpcd answers the ADSP's file requests
```

## Files

| Path | What it is | Why it is needed |
|---|---|---|
| `imx386/imx386.c` | Driver for the rear camera, Sony IMX386 (12 MP). Powers the sensor on and off, loads its registers, and gives V4L2 controls for exposure, analogue/digital gain, blanking (frame rate), flips and test patterns. 5 modes: 4032x3016 full, 4032x2256 16:9, 3840x2160 4K, 2016x1508 and 1920x1080 binned (all ~30 fps) | Mainline Linux has no IMX386 driver. The generic `ccs` driver cannot drive it because the sensor leaves its standard capability registers empty |
| `imx386/tables.h` | The register lists for start-up and for each mode | Copied as data out of Xiaomi's Android camera library on the phone (`libmmcamera_oxygen_imx386_sunny.so`); the file offsets are in its header |
| `s5k5e8/s5k5e8.c`, `s5k5e8/tables.h` | Driver + register lists for the front camera, Samsung S5K5E8 (5 MP, 2592x1944 at ~30 fps, 2 lanes) | No mainline driver. The sensor reports standard limits, but its clock (PLL) setup is Samsung-specific, so `ccs` fails on it too. Registers from `libmmcamera_oxygen_s5k5e8_*.so`, meanings checked against Xiaomi's GPL driver for the same sensor in the Redmi 6A |
| `*/Makefile` | One line each: build the file as an external module | Used by `build-modules.sh` |
| `patches/0001-…dw9763…` | Lets the mainline `dw9768` lens driver accept the DW9763 found in this phone | Same chip family and register map; only the name was missing |
| `patches/0002-…camss…` | Fix in the Qualcomm camera subsystem driver (CAMSS) for MSM8953 | The fast binned modes (1080p, 2016x1508) made CAMSS choose a 100 MHz CSI timer clock that never starts on MSM8953, so no picture came through. Without this only the slow full-resolution modes work |
| `dts/msm8953-xiaomi-oxygen-camera.dts` | Device tree = stock Mi Max 2 tree + rear camera + lens (+ front camera via `-front.dtsi`) | Tells the kernel where the camera chips are and how they are powered and wired (from the Android device tree of the phone). Mounting: rear rotated 270°, front 90° |
| `dts/msm8953-xiaomi-oxygen-front.dtsi` | The front camera part, included by the camera tree | |
| `dts/msm8953-xiaomi-oxygen-torch.dtsi` | The rear LED part: PMI8950 flash block at 0xd300, 50 mA cap, name `white:flash` | Phosh's torch button looks for an LED named `*:flash` |
| `dts/msm8953-xiaomi-oxygen-speaker.dtsi` | The loudspeaker and earpiece part: TAS2560 at 0x4c (loudspeaker, reset GPIO 20) and 0x4f (earpiece, reset GPIO 127) on I²C 2, both on the Quinary MI2S RX link (SD2), the TX link (I/V sense, SD0/SD1), and the FastRPC node of the ADSP | From the Android device tree; the FastRPC node is what lets hexagonrpcd answer the ADSP (see "Loudspeaker" in the main README) |
| `dts/…-led.dts`, `dts/…-camera-led.dts`, `dts/…-speaker.dts`, `dts/…-camera-speaker.dts`, `dts/…-led-speaker.dts`, `dts/…-camera-led-speaker.dts` | Stock tree plus each combination of camera, torch and speaker | The phone boots one device tree, so there is one per combination of installed parts; `install.sh` picks it |
| `tas2560/tas2560.c` | Driver for the TI TAS2560 loudspeaker amplifier (paged registers, reset, PLL from the bit clock, Xiaomi's power-up sequence and coefficients, I/V-sense capture; powers down on stream mute while the clocks still run, idle-channel detection off). Per amp (prefix `Speaker` / `Earpiece`): `Amp Gain Volume` (0–15 dB), `ASI Channel`, `Amp Switch` (several amps on one DAI link) and `Amp High-pass` (first-order playback filter: Off, 500 Hz, 1 kHz, 2 kHz) | Mainline has no TAS2560 driver. Written from Xiaomi's GPL driver; history in `../audio/` |
| `patches/0003-…aw2013…` | Fix in the AW2013 notification LED driver (`leds-aw2013.ko`): no register writes while the chip is powered off | With feedbackd's pattern trigger the stock driver wrote to the unpowered chip and logged `Setting an LED's brightness failed (-6)` every ~2.4 s |
| `patches/0004-…venus-hfi_parser…` | Venus (`venus-core.ko`): accept the truncated last capability entry in the firmware's init reply | Without it the stock driver fails the probe with `-EIO`, so there is no hardware video decoder at all |
| `patches/0005-…venus-msm8953-non-null-eos…` | Venus (`venus-dec.ko`, also `venc.c`): non-NULL end-of-stream buffer address on msm8953 | The firmware classifies address 0 as "inside the firmware" and marks the session compromised, so every stream hangs at its end (found by decompiling the firmware, see `../video/README.md`) |
| *(from the kernel source)* `drivers/misc/fastrpc.c` | Built unchanged as `fastrpc.ko` | The Nura kernel does not enable FastRPC (`CONFIG_QCOM_FASTRPC`) |
| *(not stored here)* `0014-pmi8950-dual-color-torch.patch` | The PMI8950 torch driver by Kostiantyn Andriiuk, which stays in his repository: [postmarketos-xiaomi-vince](https://github.com/kotXio/postmarketos-xiaomi-vince). `../fetch-upstream.sh` downloads it (pinned and checksum-checked) | The Redmi 5 Plus has the same PMIC and LED |
| `torch/0001-…optional-label.patch` | Our small change on top: the LED name can come from the device tree | So it can be called `white:flash` here |
| `torch/leds-qcom-pmi8950-torch.c` | *Generated by `../fetch-upstream.sh`, not stored here:* upstream's driver with our label patch applied | What `build-modules.sh` actually compiles |
| `torch/73-oxygen-flash-not-notification.rules` | udev rule | Stops feedbackd from blinking the camera LED for notifications (the phone has a separate notification LED) |
| `build-modules.sh` | Builds all modules and the seven device trees, on a PC in Docker or on the phone (see below) | Reproducible build of `../prebuilt/kernel/`, and builds for newer kernels |
| `config-7.0.9-msm8953` | The configuration of the Nura v26.06 kernel | Modules must be built with exactly the running kernel's configuration |

## Build

**On the phone**, for the kernel it runs. `install.sh --newversion` does this by itself:

```sh
sudo sh build-modules.sh --phone
```

It looks up which msm8953-mainline tag Nura's kernel package is built from (and its
checksum) in Nura's package recipe, downloads it, applies `patches/`, takes the running
kernel's configuration from `/proc/config.gz`, and checks that the result is exactly the
running kernel release. Then it only *prepares* the kernel tree (not a full kernel build:
without symbol versioning a module only needs the same release and configuration),
builds the seven modules and the seven device trees into `out/<kernel release>/`, and
removes the build tools and the ~2 GB work folder again.

**On a PC** (full kernel build in Docker, ~15 min on 16 cores):

```sh
docker run --rm -v "$PWD:/w" alpine:3.24 sh /w/build-modules.sh
```

With the defaults this rebuilds `../prebuilt/kernel/` (the device trees come out
byte-identical). For another kernel pass `-e TAG=… -e CONFIG=…`.

If a patch no longer applies, the build stops and names it; a patch that is already
part of the new kernel is skipped.
