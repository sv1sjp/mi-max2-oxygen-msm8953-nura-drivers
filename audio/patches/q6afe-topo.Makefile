# SPDX-License-Identifier: GPL-2.0
obj-m += q6afe_topo.o
q6afe_topo-y := q6afe.o
ccflags-y += -I$(srctree)/sound/soc/qcom/qdsp6
