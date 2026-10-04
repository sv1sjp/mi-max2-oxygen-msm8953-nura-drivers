// SPDX-License-Identifier: GPL-2.0
/*
 * Debug only: read-modify-write one LPASS CSR audio mux register on msm8953
 * (mic-iomux 0xc051000, spkr-iomux 0xc051004, quin-iomux 0xc052000,
 * pri pcm/mi2s muxsel 0xc055000). Prints all four before/after.
 * usage: insmod lpassmux.ko addr=0x0c051004 clr=0x30000 set=0
 */
#include <linux/io.h>
#include <linux/module.h>

static unsigned int addr;
module_param(addr, uint, 0444);
static unsigned int clr;
module_param(clr, uint, 0444);
static unsigned int set;
module_param(set, uint, 0444);

static const unsigned int regs[] = { 0x0c051000, 0x0c051004, 0x0c052000, 0x0c055000 };

static void dump(const char *when)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		void __iomem *p = ioremap(regs[i], 4);

		if (!p)
			continue;
		pr_info("lpassmux %s: 0x%08x = 0x%08x\n", when, regs[i], readl(p));
		iounmap(p);
	}
}

static int __init lpassmux_init(void)
{
	dump("before");
	if (addr == 0x0c051000 || addr == 0x0c051004 || addr == 0x0c052000 || addr == 0x0c055000) {
		void __iomem *p = ioremap(addr, 4);

		if (p) {
			writel((readl(p) & ~clr) | set, p);
			iounmap(p);
		}
		dump("after");
	}
	return -EAGAIN;
}
module_init(lpassmux_init);
MODULE_DESCRIPTION("debug: poke msm8953 LPASS audio mux registers");
MODULE_LICENSE("GPL");
