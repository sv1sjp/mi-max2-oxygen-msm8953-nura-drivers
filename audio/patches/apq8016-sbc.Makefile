# SPDX-License-Identifier: GPL-2.0
obj-m += snd-soc-apq8016-sbc-test.o
snd-soc-apq8016-sbc-test-y := apq8016_sbc.o
ccflags-y += -I$(srctree)/sound/soc/qcom
