#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
M=/lib/modules/$(uname -r)/kernel/sound/soc/qcom/qdsp6
echo "== before"; lsmod | grep -E "^(q6afe|q6adm|q6routing|q6afe_dai)" ; cat /proc/asound/cards
insmod /home/user/q6afe_topo.ko && echo "q6afe_topo loaded"
for m in q6afe-clocks q6afe-dai q6adm q6routing q6asm-dai q6voice q6voice-dai; do insmod $M/$m.ko 2>/dev/null && echo "loaded $m"; done
modprobe snd_soc_apq8016_sbc
insmod /home/user/tas2560.ko
sleep 6; cat /proc/asound/cards | head -1
