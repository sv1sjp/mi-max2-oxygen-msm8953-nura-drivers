# prebuilt/: ready-made files for Nura v26.06

What `install.sh` copies to the phone, already compiled so nobody needs to build
anything. They fit **only** kernel `7.0.9-msm8953` (package
`linux-postmarketos-qcom-msm8953-7.0.9-r0`) and libcamera 0.7.1 (package
`libcamera-99990.7.1-r0`), on aarch64. For other versions see "Newer Nura versions"
in the main README.

| Path | Built by | Installed to |
|---|---|---|
| `kernel/imx386.ko`, `s5k5e8.ko`, `dw9768.ko`, `qcom-camss.ko` | `../kernel/build-modules.sh` | `/lib/modules/7.0.9-msm8953/updates/` (camera) |
| `kernel/leds-qcom-pmi8950-torch.ko` | same | same (LED) |
| `kernel/camera.dtb`, `led.dtb`, `camera-led.dtb` | same | one of them, depending on what is installed, to `/boot/msm8953-xiaomi-oxygen.dtb` and `/boot/dtbs/qcom/msm8953-xiaomi-oxygen.dtb` |
| `libcamera/usr/...` | `../libcamera/prepare-src.sh` + `xbuild.sh` | the same paths on the phone: this folder is laid out like the phone's filesystem |

`libcamera/usr/share/libcamera/ipa/simple/*.yaml` are copies of `../libcamera/*.yaml`.

The source of every file is in this repository, except the third-party parts, which stay in their authors' repositories and are downloaded by `../fetch-upstream.sh` (see "Third-party files" in the main README). Rebuilding gives the same device
trees byte for byte. The modules differ only in embedded build paths. Every file is
listed in `../SHA256SUMS`, and `install.sh` refuses to run if one was changed.
