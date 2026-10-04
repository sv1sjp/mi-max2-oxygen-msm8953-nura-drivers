// SPDX-License-Identifier: GPL-2.0
/*
 * DSP-less Quinary MI2S test for msm8953 (Xiaomi Mi Max 2).
 *
 * Question: can the Quinary MI2S interface generate a word clock (WS) at all
 * when Linux drives it directly, bypassing the ADSP (whose Quinary START hangs)?
 *
 * Steps (each value copied from what the ADSP itself programmed for the working
 * Quaternary port, test #22/#24):
 *   1. Quinary bit clock: RCG @0x0c032000 (CFG 0x2513, M 1, N 0xf8, D 0xf7,
 *      update), branch CBCR @0x0c032018 enable. Bounded polls only.
 *   2. quin_iomux (0x0c052000) bit0 = route Quinary to the pri_mi2s pads;
 *      gpio92 -> pri_mi2s_ws (func 1, 8 mA).
 *   3. I2S_CTL[slot] (0x0c0c4000 + slot*0x1000) = 0x000f4400
 *      (SPKEN, SPKMODE=SD0, WSSRC=internal, 16 bit) - the value the ADSP wrote
 *      for the running Quaternary port.
 *   4. Sample pads gpio88..95 (TLMM GPIO_IN) and report which toggle.
 *   5. Restore every register to its original value.
 * Only single known registers are accessed (never the whole LPAIF window).
 * Must run while another stream (e.g. headphones/earpiece) keeps LPASS clocked.
 * Loads, reports, restores, unloads (-EAGAIN).
 *   insmod quinws.ko slot=5            (dry=1: only print current registers)
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>

static unsigned int slot = 5;
module_param(slot, uint, 0444);
static bool dry;
module_param(dry, bool, 0444);
static unsigned int i2sval = 0x000f4400;
module_param(i2sval, uint, 0444);

#define QUI_RCG		0x0c032000	/* CMD, +4 CFG, +8 M, +0xc N, +0x10 D */
#define QUI_CBCR	0x0c032018
#define QUIN_IOMUX	0x0c052000
#define I2S_CTL(n)	(0x0c0c4000 + (n) * 0x1000)
#define TLMM_CFG(g)	(0x01000000 + (g) * 0x1000)
#define TLMM_IO(g)	(0x01000004 + (g) * 0x1000)

static u32 rd(phys_addr_t a)
{
	void __iomem *p = ioremap(a, 4);
	u32 v = p ? readl(p) : 0xdeadbeef;

	if (p)
		iounmap(p);
	return v;
}

static void wr(phys_addr_t a, u32 v)
{
	void __iomem *p = ioremap(a, 4);

	if (p) {
		writel(v, p);
		iounmap(p);
	}
}

static void sample(const char *tag)
{
	unsigned int ones[8] = { 0 }, edges[8] = { 0 }, last[8], i, g;
	void __iomem *b = ioremap(TLMM_CFG(88), 8 * 0x1000);

	if (!b)
		return;
	for (g = 0; g < 8; g++)
		last[g] = readl(b + g * 0x1000 + 4) & 1;
	for (i = 0; i < 200000; i++)
		for (g = 0; g < 8; g++) {
			unsigned int v = readl(b + g * 0x1000 + 4) & 1;

			ones[g] += v;
			edges[g] += v != last[g];
			last[g] = v;
		}
	for (g = 0; g < 8; g++)
		if (edges[g] > 100)
			pr_info("quinws %s: gpio%u ACTIVE ones=%u edges=%u\n", tag, 88 + g, ones[g], edges[g]);
	pr_info("quinws %s: sampling done\n", tag);
	iounmap(b);
}

static int __init quinws_init(void)
{
	u32 rcg[5], cbcr, iomux, g92, ctl[7], v;
	unsigned int i;

	if (slot > 6)
		return -EINVAL;
	for (i = 0; i < 5; i++)
		rcg[i] = rd(QUI_RCG + i * 4);
	cbcr = rd(QUI_CBCR);
	iomux = rd(QUIN_IOMUX);
	g92 = rd(TLMM_CFG(92));
	for (i = 0; i < 7; i++)
		ctl[i] = rd(I2S_CTL(i));
	pr_info("quinws: before: rcg %08x %08x %08x %08x %08x cbcr %08x quin_iomux %08x gpio92cfg %03x\n",
		rcg[0], rcg[1], rcg[2], rcg[3], rcg[4], cbcr, iomux, g92);
	pr_info("quinws: before: i2s_ctl %08x %08x %08x %08x %08x %08x %08x\n",
		ctl[0], ctl[1], ctl[2], ctl[3], ctl[4], ctl[5], ctl[6]);
	if (dry)
		return -EAGAIN;

	/* 1. Quinary bit clock 1.536 MHz, same divider as the ADSP used for Quaternary */
	wr(QUI_RCG + 4, 0x2513);
	wr(QUI_RCG + 8, 0x1);
	wr(QUI_RCG + 0xc, 0xf8);
	wr(QUI_RCG + 0x10, 0xf7);
	wr(QUI_RCG, rd(QUI_RCG) | 1);		/* UPDATE */
	for (i = 0; i < 1000 && (rd(QUI_RCG) & 1); i++)
		udelay(1);
	wr(QUI_CBCR, rd(QUI_CBCR) | 1);		/* CLK_ENABLE */
	for (i = 0; i < 1000 && (rd(QUI_CBCR) & BIT(31)); i++)
		udelay(1);
	pr_info("quinws: clock: rcg cmd %08x cbcr %08x (bit31=CLK_OFF)\n", rd(QUI_RCG), rd(QUI_CBCR));

	/* 2. pads */
	wr(QUIN_IOMUX, iomux | 1);
	wr(TLMM_CFG(92), (g92 & ~0x3fc) | (1 << 2) | (3 << 6));	/* func 1, 8 mA, no pull */
	pr_info("quinws: quin_iomux %08x gpio92cfg %03x\n", rd(QUIN_IOMUX), rd(TLMM_CFG(92)));
	sample("clock-only");

	/* 3. enable the interface (speaker path) */
	wr(I2S_CTL(slot), i2sval);
	v = rd(I2S_CTL(slot));
	pr_info("quinws: i2s_ctl[%u] = %08x (wrote %08x)\n", slot, v, i2sval);
	udelay(1000);
	sample("interface-on");

	/* 5. restore */
	wr(I2S_CTL(slot), ctl[slot]);
	wr(TLMM_CFG(92), g92);
	wr(QUIN_IOMUX, iomux);
	wr(QUI_CBCR, cbcr);
	for (i = 1; i < 5; i++)
		wr(QUI_RCG + i * 4, rcg[i]);
	wr(QUI_RCG, rd(QUI_RCG) | 1);
	pr_info("quinws: restored: i2s_ctl[%u] %08x cbcr %08x quin_iomux %08x gpio92cfg %03x\n",
		slot, rd(I2S_CTL(slot)), rd(QUI_CBCR), rd(QUIN_IOMUX), rd(TLMM_CFG(92)));
	return -EAGAIN;
}
module_init(quinws_init);
MODULE_DESCRIPTION("debug: DSP-less Quinary MI2S word-clock test (msm8953)");
MODULE_LICENSE("GPL");
