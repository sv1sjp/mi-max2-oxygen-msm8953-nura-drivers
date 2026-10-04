// SPDX-License-Identifier: GPL-2.0
/*
 * Debug only: read (and optionally clear bit 3 of) the ADSP QDSP6SS register
 * at 0x0c20002c. On SDM632 (Fairphone 3) mainline PAS leaves this bit set while
 * downstream PIL leaves it clear, and with it set an LPASS interface (SLIMbus
 * framer) never syncs. Loads, prints, optionally clears, then unloads (-EAGAIN).
 */
#include <linux/bits.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/module.h>

static bool clear;
module_param(clear, bool, 0444);
static unsigned int hold_ms;
module_param(hold_ms, uint, 0444);
MODULE_PARM_DESC(hold_ms, "keep clearing bit 3 every ~50us for this many ms (0 = once)");
MODULE_PARM_DESC(clear, "clear bit 3 of 0x0c20002c");

#define QDSP6SS_BASE	0x0c200000
#define REG_OFF		0x2c

static int __init qdsp6ss_bit_init(void)
{
	void __iomem *base = ioremap(QDSP6SS_BASE, 0x100);
	unsigned int off;
	u32 v;

	if (!base)
		return -ENOMEM;

	for (off = 0; off < 0x40; off += 4)
		pr_info("qdsp6ss_bit: 0x%08x = 0x%08x\n", QDSP6SS_BASE + off,
			readl(base + off));

	v = readl(base + REG_OFF);
	pr_info("qdsp6ss_bit: reg 0x%08x = 0x%08x (bit3=%d)\n",
		QDSP6SS_BASE + REG_OFF, v, !!(v & BIT(3)));
	if (clear && (v & BIT(3))) {
		writel(v & ~BIT(3), base + REG_OFF);
		v = readl(base + REG_OFF);
		pr_info("qdsp6ss_bit: cleared -> 0x%08x (bit3=%d)\n", v, !!(v & BIT(3)));
	}

	if (clear && hold_ms) {
		unsigned long end = jiffies + msecs_to_jiffies(hold_ms);
		unsigned int loops = 0, resets = 0;

		while (time_before(jiffies, end)) {
			v = readl(base + REG_OFF);
			if (v & BIT(3)) {
				resets++;
				writel(v & ~BIT(3), base + REG_OFF);
			}
			loops++;
			usleep_range(40, 60);
		}
		pr_info("qdsp6ss_bit: hold %u ms: %u checks, bit3 re-set %u times, now 0x%08x\n",
			hold_ms, loops, resets, readl(base + REG_OFF));
	}

	iounmap(base);
	return -EAGAIN;
}
module_init(qdsp6ss_bit_init);
MODULE_DESCRIPTION("debug: QDSP6SS 0x0c20002c bit 3");
MODULE_LICENSE("GPL");
