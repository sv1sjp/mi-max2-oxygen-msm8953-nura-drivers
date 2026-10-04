// SPDX-License-Identifier: GPL-2.0
/*
 * DSP-less Quinary MI2S DMA feasibility test (msm8953, Mi Max 2).
 *
 * Question: can Linux drive an LPAIF read-DMA channel to stream samples into the
 * Quinary interface (slot 5), producing DATA on the amp's data pad (gpio88/SD0)?
 * If yes, a Linux-only Quinary speaker driver is feasible (the ADSP's own Quinary
 * start hangs on this board; the interface itself works - proven by quinws.ko).
 *
 * Safe design: no buffer allocation, no writes to memory the DSP owns. It reuses
 * the LPASS on-chip buffer (LLB) that the DSP's earpiece read-DMA is already
 * filling. So run it while a TONE plays on the earpiece (varying data in LLB).
 * It points an IDLE read-DMA channel (default ch1) at that same LLB buffer,
 * routed to Quinary, sweeps the interface-select field, and samples gpio88.
 * Whichever field value makes gpio88 toggle is Quinary's DMA interface id.
 * Restores every register. Loads, reports, unloads (-EAGAIN).
 *   insmod quindma.ko           (ch=1 by default, sweep intf 0..15)
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>

static unsigned int ch = 1;		/* idle read-DMA channel to borrow */
module_param(ch, uint, 0444);

#define QUI_RCG		0x0c032000	/* CMD +0, CFG +4, M +8, N +0xc, D +0x10 */
#define QUI_CBCR	0x0c032018
#define QUIN_IOMUX	0x0c052000
#define I2S_CTL5	0x0c0c9000	/* Quinary interface (slot 5) */
#define RDMA(c)		(0x0c0d2000 + (c) * 0x1000)	/* read-DMA channel c */
#define RDMA_CTL(c)	(RDMA(c) + 0x0)
#define RDMA_BASE(c)	(RDMA(c) + 0x4)
#define RDMA_BUFF(c)	(RDMA(c) + 0x8)
#define RDMA_PER(c)	(RDMA(c) + 0x10)
#define TLMM_CFG(g)	(0x01000000 + (g) * 0x1000)
#define TLMM_IN(g)	(0x01000004 + (g) * 0x1000)

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

/* sample gpio88 (SD0 data) for toggling; returns edge count */
static unsigned int edgesg(unsigned int g)
{
	void __iomem *p = ioremap(TLMM_IN(g), 4);
	unsigned int i, e = 0, last;

	if (!p)
		return 0;
	last = readl(p) & 1;
	for (i = 0; i < 120000; i++) {
		unsigned int v = readl(p) & 1;

		e += v != last;
		last = v;
	}
	iounmap(p);
	return e;
}

static int __init quindma_init(void)
{
	u32 s_rcg[5], s_cbcr, s_iomux, s_g88, s_g91, s_g92, s_ctl5, s_dctl, s_dbase, s_dbuff, s_dper;
	u32 base0, buff0, per0, ctl0;
	unsigned int i, intf;

	if (ch < 1 || ch > 3)
		return -EINVAL;

	/* save */
	for (i = 0; i < 5; i++)
		s_rcg[i] = rd(QUI_RCG + i * 4);
	s_cbcr = rd(QUI_CBCR); s_iomux = rd(QUIN_IOMUX); s_ctl5 = rd(I2S_CTL5);
	s_g88 = rd(TLMM_CFG(88)); s_g91 = rd(TLMM_CFG(91)); s_g92 = rd(TLMM_CFG(92));
	s_dctl = rd(RDMA_CTL(ch)); s_dbase = rd(RDMA_BASE(ch));
	s_dbuff = rd(RDMA_BUFF(ch)); s_dper = rd(RDMA_PER(ch));

	/* the DSP's earpiece read-DMA is ch0: borrow its LLB buffer (varying data) */
	base0 = rd(RDMA_BASE(0)); buff0 = rd(RDMA_BUFF(0));
	per0 = rd(RDMA_PER(0)); ctl0 = rd(RDMA_CTL(0));
	pr_info("quindma: ch0 (earpiece) CTL=%08x BASE=%08x BUFF=%08x PER=%08x\n",
		ctl0, base0, buff0, per0);
	if (!(ctl0 & 1) || !base0) {
		pr_info("quindma: ch0 not active - need a TONE playing on the earpiece. abort.\n");
		return -EAGAIN;
	}
	if (rd(RDMA_CTL(ch)) & 1) {
		pr_info("quindma: chosen ch%u is busy, abort.\n", ch);
		return -EAGAIN;
	}

	/* 1. Quinary bit clock (same divider the ADSP uses; quinws-proven) */
	wr(QUI_RCG + 4, 0x2513); wr(QUI_RCG + 8, 1); wr(QUI_RCG + 0xc, 0xf8); wr(QUI_RCG + 0x10, 0xf7);
	wr(QUI_RCG, rd(QUI_RCG) | 1);
	for (i = 0; i < 1000 && (rd(QUI_RCG) & 1); i++) udelay(1);
	wr(QUI_CBCR, rd(QUI_CBCR) | 1);
	for (i = 0; i < 1000 && (rd(QUI_CBCR) & BIT(31)); i++) udelay(1);

	/* 2. pads: quin iomux + gpio91 BCLK, gpio92 WS, gpio88 SD0 data (func1, 8mA) */
	wr(QUIN_IOMUX, s_iomux | 1);
	wr(TLMM_CFG(91), (s_g91 & ~0x3fc) | (1 << 2) | (3 << 6));
	wr(TLMM_CFG(92), (s_g92 & ~0x3fc) | (1 << 2) | (3 << 6));
	wr(TLMM_CFG(88), (s_g88 & ~0x3fc) | (1 << 2) | (3 << 6));

	/* 3. Quinary interface on: SPKEN, SD0, WS internal, 16-bit (quinws-proven WS) */
	wr(I2S_CTL5, 0x000f4400);

	/* 4. point the idle DMA channel at the earpiece's LLB buffer */
	wr(RDMA_BASE(ch), base0);
	wr(RDMA_BUFF(ch), buff0);
	wr(RDMA_PER(ch), per0);

	/* 5. sweep interface-select (CTL bits 10-13) and watch gpio88 */
	pr_info("quindma: sweeping intf; report CURR advance (DMA alive) + gpio88/93 data\n");
	for (intf = 0; intf < 16; intf++) {
		u32 c1, c2, e88, e93;

		wr(RDMA_CTL(ch), 1 | (7 << 1) | (intf << 10));
		c1 = rd(RDMA(ch) + 0xc);
		udelay(3000);
		c2 = rd(RDMA(ch) + 0xc);
		e88 = edgesg(88);
		e93 = edgesg(93);
		pr_info("quindma:   intf=%2u CTL=%08x CURR %08x->%08x %s  gpio88=%u gpio93=%u%s\n",
			intf, rd(RDMA_CTL(ch)), c1, c2,
			c2 != c1 ? "ADVANCING" : "stuck", e88, e93,
			(e88 > 100 || e93 > 100) ? "  <-- DATA" : "");
		wr(RDMA_CTL(ch), 0);
		udelay(500);
	}

	/* 6. restore */
	wr(RDMA_CTL(ch), s_dctl); wr(RDMA_BASE(ch), s_dbase);
	wr(RDMA_BUFF(ch), s_dbuff); wr(RDMA_PER(ch), s_dper);
	wr(I2S_CTL5, s_ctl5);
	wr(TLMM_CFG(88), s_g88); wr(TLMM_CFG(91), s_g91); wr(TLMM_CFG(92), s_g92);
	wr(QUIN_IOMUX, s_iomux);
	wr(QUI_CBCR, s_cbcr);
	for (i = 1; i < 5; i++)
		wr(QUI_RCG + i * 4, s_rcg[i]);
	wr(QUI_RCG, rd(QUI_RCG) | 1);
	pr_info("quindma: restored. done.\n");
	return -EAGAIN;
}
module_init(quindma_init);
MODULE_DESCRIPTION("debug: DSP-less Quinary MI2S DMA feasibility test (msm8953)");
MODULE_LICENSE("GPL");
