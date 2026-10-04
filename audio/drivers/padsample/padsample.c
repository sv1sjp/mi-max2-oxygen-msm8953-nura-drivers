// SPDX-License-Identifier: GPL-2.0
/*
 * Debug only: sample the input level of msm8953 TLMM pads (default 88..95, the
 * pri_mi2s pads) many times and report per pad: CFG register, number of 1s,
 * number of level changes. A pad carrying I2S clock/data toggles; an idle one
 * does not. Read-only. Loads, prints, unloads (-EAGAIN).
 */
#include <linux/io.h>
#include <linux/module.h>

#define TLMM_BASE	0x01000000
#define TLMM_STRIDE	0x1000
#define NPADS		8

static unsigned int first = 88;
module_param(first, uint, 0444);
static unsigned int samples = 200000;
module_param(samples, uint, 0444);
static char *tag = "";
module_param(tag, charp, 0444);

static int __init padsample_init(void)
{
	void __iomem *base = ioremap(TLMM_BASE + first * TLMM_STRIDE, NPADS * TLMM_STRIDE);
	unsigned int ones[NPADS] = { 0 }, edges[NPADS] = { 0 }, last[NPADS];
	unsigned int i, p;

	if (!base)
		return -ENOMEM;

	for (p = 0; p < NPADS; p++)
		last[p] = readl(base + p * TLMM_STRIDE + 4) & 1;

	for (i = 0; i < samples; i++) {
		for (p = 0; p < NPADS; p++) {
			unsigned int v = readl(base + p * TLMM_STRIDE + 4) & 1;

			ones[p] += v;
			edges[p] += v != last[p];
			last[p] = v;
		}
	}

	for (p = 0; p < NPADS; p++)
		pr_info("padsample %s: gpio%u cfg=0x%03x func=%u ones=%u/%u edges=%u %s\n",
			tag, first + p, readl(base + p * TLMM_STRIDE) & 0x7ff,
			(readl(base + p * TLMM_STRIDE) >> 2) & 0xf, ones[p], samples,
			edges[p], edges[p] > 100 ? "ACTIVE" : "idle");

	iounmap(base);
	return -EAGAIN;
}
module_init(padsample_init);
MODULE_DESCRIPTION("debug: sample msm8953 pri_mi2s pad activity");
MODULE_LICENSE("GPL");
