# SPDX-License-Identifier: GPL-2.0-or-later
# Xiaomi Mi Max 2 (oxygen): draw GTK4 apps on the GPU (Adreno 506) again.
# Overrides GSK_RENDERER=cairo from soc-qcom-msm8953-gpu's adreno-a506-quirks.sh,
# which this file sorts after. With GTK 4.22 the ngl renderer is fast on the a506
# (Snapshot preview: ~0.25 instead of ~1.5 CPU cores). See gpu/README.md.
export GSK_RENDERER=ngl
