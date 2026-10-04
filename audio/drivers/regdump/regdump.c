// SPDX-License-Identifier: GPL-2.0
/* Debug only: print the LPASS CSR audio mux registers on msm8953. */
#include <linux/io.h>
#include <linux/module.h>

static const struct {
	const char *name;
	phys_addr_t addr;
} regs[] = {
	{ "mic-iomux  (0xc051000)", 0x0c051000 },
	{ "spkr-iomux (0xc051004)", 0x0c051004 },
	{ "quin-iomux (0xc052000)", 0x0c052000 },
	{ "pri-iomux  (0xc055000)", 0x0c055000 },
};

static int __init regdump_init(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		void __iomem *p = ioremap(regs[i].addr, 4);

		if (!p)
			continue;
		pr_info("regdump: %s = 0x%08x\n", regs[i].name, readl(p));
		iounmap(p);
	}

	return -EAGAIN;	/* print and unload */
}
module_init(regdump_init);
MODULE_LICENSE("GPL");
