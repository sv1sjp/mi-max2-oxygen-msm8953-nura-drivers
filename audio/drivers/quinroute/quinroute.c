// SPDX-License-Identifier: GPL-2.0
/*
 * DSP-less Quinary MI2S bring-up v2 (msm8953, Mi Max 2).
 *
 * v1 wrote routing reg 0x0c0d000c and the DMA never drained (interface not fed).
 * A test-boot snapshot of WORKING Quaternary playback (regsnap rt-quat.bin) revealed
 * the real recipe (see QUINARY-DRIVER-DESIGN.md "Quaternary capture"):
 *   - read-DMA CTL has an INTERFACE-SELECT field at bits 6-8 (mask 0x1c0):
 *       earpiece/internal-codec = 1, Quaternary = 4  => value = I2S_slot + 1
 *       so Quinary (I2S slot 5) is predicted 6 (MI2S-index alt would be 5) -> sweep 1..7.
 *   - routing enables: 0x0c0d0008 |= 1, 0x0c0d1000 |= 7 (present in every working path).
 *   - DMA CTL also carries watermark (bits1-5), words-per-sample count (bits10-13),
 *     width (bits15-16); Quaternary's live CTL was 0x610f -> reuse its non-intf bits.
 * v1 (and quindma) left bits6-8 = 0, i.e. interface NONE -> that is why it never drained.
 *
 * This borrows the earpiece read-DMA's LLB buffer (varying data), uses an IDLE channel,
 * enables I2S_CTL[5] for the amp's SD2/SD3 pads (gpio94/95), sweeps the DMA interface-
 * select field, and samples gpio88/93/94/95. A pad that toggles while CURR keeps
 * advancing = the DMA is clocking into Quinary = the real speaker data path.
 * Amp stays UNPOWERED (no I2C), so nothing is loud; this only proves the data path.
 * Saves/restores every register. Run while a TONE plays on the earpiece.
 *   insmod quinroute.ko            (ch=1 idle channel; spkmode=6 QUAD23 default)
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>

static unsigned int ch = 1;
module_param(ch, uint, 0444);
static unsigned int spkmode = 6;	/* 1=SD0 3=SD2 4=SD3 6=QUAD23 (amp on SD2/SD3) */
module_param(spkmode, uint, 0444);

#define QUI_RCG		0x0c032000
#define QUI_CBCR	0x0c032018
#define QUIN_IOMUX	0x0c052000
#define I2S_CTL5	0x0c0c9000
#define ROUTE_08	0x0c0d0008
#define ROUTE_1000	0x0c0d1000
#define RDMA(c)		(0x0c0d2000 + (c) * 0x1000)
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

static unsigned int edgesg(unsigned int g)
{
	void __iomem *p = ioremap(TLMM_IN(g), 4);
	unsigned int i, e = 0, last;

	if (!p)
		return 0;
	last = readl(p) & 1;
	for (i = 0; i < 80000; i++) {
		unsigned int v = readl(p) & 1;

		e += v != last;
		last = v;
	}
	iounmap(p);
	return e;
}

static int __init quinroute_init(void)
{
	u32 s_rcg[5], s_cbcr, s_iomux, s_ctl5, s_r08, s_r1000;
	u32 s_g88, s_g91, s_g92, s_g93, s_g94, s_g95;
	u32 s_dctl, s_dbase, s_dbuff, s_dper;
	u32 base0, buff0, per0, ctl0, ctl5, intf;
	unsigned int i;

	if (ch < 1 || ch > 3 || spkmode > 7)
		return -EINVAL;

	ctl0 = rd(RDMA(0) + 0); base0 = rd(RDMA(0) + 4);
	buff0 = rd(RDMA(0) + 8); per0 = rd(RDMA(0) + 0x10);
	pr_info("quinroute2: ch0(earpiece) CTL=%08x BASE=%08x BUFF=%08x PER=%08x\n",
		ctl0, base0, buff0, per0);
	if (!(ctl0 & 1) || !base0) {
		pr_info("quinroute2: ch0 idle - need a TONE on the earpiece. abort.\n");
		return -EAGAIN;
	}
	if (rd(RDMA(ch) + 0) & 1) {
		pr_info("quinroute2: ch%u busy, abort.\n", ch);
		return -EAGAIN;
	}

	for (i = 0; i < 5; i++)
		s_rcg[i] = rd(QUI_RCG + i * 4);
	s_cbcr = rd(QUI_CBCR); s_iomux = rd(QUIN_IOMUX); s_ctl5 = rd(I2S_CTL5);
	s_r08 = rd(ROUTE_08); s_r1000 = rd(ROUTE_1000);
	s_g88 = rd(TLMM_CFG(88)); s_g91 = rd(TLMM_CFG(91)); s_g92 = rd(TLMM_CFG(92));
	s_g93 = rd(TLMM_CFG(93)); s_g94 = rd(TLMM_CFG(94)); s_g95 = rd(TLMM_CFG(95));
	s_dctl = rd(RDMA(ch) + 0); s_dbase = rd(RDMA(ch) + 4);
	s_dbuff = rd(RDMA(ch) + 8); s_dper = rd(RDMA(ch) + 0x10);
	pr_info("quinroute2: before: 0x0c0d0008=%08x 0x0c0d1000=%08x\n", s_r08, s_r1000);

	/* Quinary bit clock (quinws-proven divider) */
	wr(QUI_RCG + 4, 0x2513); wr(QUI_RCG + 8, 1); wr(QUI_RCG + 0xc, 0xf8); wr(QUI_RCG + 0x10, 0xf7);
	wr(QUI_RCG, rd(QUI_RCG) | 1);
	for (i = 0; i < 1000 && (rd(QUI_RCG) & 1); i++) udelay(1);
	wr(QUI_CBCR, rd(QUI_CBCR) | 1);
	for (i = 0; i < 1000 && (rd(QUI_CBCR) & BIT(31)); i++) udelay(1);

	/* pads: quin iomux + BCLK(91) WS(92) + data 88/93 (func1) + 94/95 SD2/SD3 (func2), 8mA */
	wr(QUIN_IOMUX, s_iomux | 1);
	wr(TLMM_CFG(91), (s_g91 & ~0x3fc) | (1 << 2) | (3 << 6));
	wr(TLMM_CFG(92), (s_g92 & ~0x3fc) | (1 << 2) | (3 << 6));
	wr(TLMM_CFG(88), (s_g88 & ~0x3fc) | (1 << 2) | (3 << 6));
	wr(TLMM_CFG(93), (s_g93 & ~0x3fc) | (1 << 2) | (3 << 6));
	wr(TLMM_CFG(94), (s_g94 & ~0x3fc) | (2 << 2) | (3 << 6));
	wr(TLMM_CFG(95), (s_g95 & ~0x3fc) | (2 << 2) | (3 << 6));

	/* use the PROVEN interface value: SD0, SPKEN (quinws showed this makes WS on gpio92,
	 * and it is exactly what the DSP wrote for the working Quaternary interface). */
	ctl5 = 0x000f4400;
	(void)spkmode;

	/* routing enables observed on every working path */
	wr(ROUTE_08, s_r08 | 0x1);
	wr(ROUTE_1000, s_r1000 | 0x7);

	/* point the idle DMA channel at the earpiece LLB buffer */
	wr(RDMA(ch) + 4, base0);
	wr(RDMA(ch) + 8, buff0);
	wr(RDMA(ch) + 0x10, per0);

	pr_info("quinroute2: interface value=%08x (SD0). DMA-FIRST order; sweeping intf-select\n", ctl5);
	for (intf = 1; intf <= 7; intf++) {
		u32 dctl = 0x6000 | (intf << 6) | (7 << 1) | 1;	/* like quat 0x610f, intf swept */
		u32 c1, c2, c3, e88, e92, e93, e94, e95;

		/* CORRECT ORDER: start the DMA first, then enable the interface (SPKEN) */
		wr(I2S_CTL5, 0x000f0004);	/* interface disabled/reset first */
		wr(RDMA(ch) + 0, dctl);		/* DMA on, with interface-select */
		udelay(200);
		wr(I2S_CTL5, ctl5);		/* now SPKEN -> interface starts pulling FIFO */
		c1 = rd(RDMA(ch) + 0xc);
		udelay(2000);
		c2 = rd(RDMA(ch) + 0xc);
		udelay(2000);
		c3 = rd(RDMA(ch) + 0xc);
		e88 = edgesg(88); e92 = edgesg(92); e93 = edgesg(93);
		e94 = edgesg(94); e95 = edgesg(95);
		pr_info("quinroute2: intf=%u CTL=%08x CURR %08x->%08x->%08x %s WS(92)=%u data[88=%u 93=%u 94=%u 95=%u]%s\n",
			intf, rd(RDMA(ch) + 0), c1, c2, c3,
			(c3 != c2 && c2 != c1) ? "STEADY-DRAIN" : (c2 != c1 ? "onestep" : "stuck"),
			e92, e88, e93, e94, e95,
			(e88 > 80 || e93 > 80 || e94 > 80 || e95 > 80) ? "  <== DATA!" : "");
		wr(I2S_CTL5, 0x000f0004);
		wr(RDMA(ch) + 0, 0);
		udelay(300);
	}

	/* restore */
	wr(RDMA(ch) + 0, s_dctl); wr(RDMA(ch) + 4, s_dbase);
	wr(RDMA(ch) + 8, s_dbuff); wr(RDMA(ch) + 0x10, s_dper);
	wr(ROUTE_1000, s_r1000); wr(ROUTE_08, s_r08);
	wr(I2S_CTL5, s_ctl5);
	wr(TLMM_CFG(88), s_g88); wr(TLMM_CFG(91), s_g91); wr(TLMM_CFG(92), s_g92);
	wr(TLMM_CFG(93), s_g93); wr(TLMM_CFG(94), s_g94); wr(TLMM_CFG(95), s_g95);
	wr(QUIN_IOMUX, s_iomux);
	wr(QUI_CBCR, s_cbcr);
	for (i = 1; i < 5; i++)
		wr(QUI_RCG + i * 4, s_rcg[i]);
	wr(QUI_RCG, rd(QUI_RCG) | 1);
	pr_info("quinroute2: restored 0x0c0d0008=%08x 0x0c0d1000=%08x. done.\n",
		rd(ROUTE_08), rd(ROUTE_1000));
	return -EAGAIN;
}
module_init(quinroute_init);
MODULE_DESCRIPTION("debug: DSP-less Quinary MI2S bring-up v2 (msm8953)");
MODULE_LICENSE("GPL");
