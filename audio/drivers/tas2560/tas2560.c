// SPDX-License-Identifier: GPL-2.0
/*
 * Texas Instruments TAS2560 mono Class-D smart amplifier driver
 *
 * Register sequences are based on the TAS2560 driver in the Xiaomi Mi Max 2
 * (oxygen) kernel sources, Copyright (c) 2016 Texas Instruments Inc.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

/* Registers are addressed as page * 128 + reg, book 0 only */
#define TAS2560_REG(page, reg)		((page) * 128 + (reg))

#define TAS2560_PAGE_SEL		TAS2560_REG(0, 0)
#define TAS2560_SW_RESET		TAS2560_REG(0, 1)
#define TAS2560_DEV_MODE		TAS2560_REG(0, 2)
#define TAS2560_SPK_CTRL		TAS2560_REG(0, 4)
#define TAS2560_SPK_GAIN_MASK		GENMASK(3, 0)
#define TAS2560_PWR			TAS2560_REG(0, 7)
#define TAS2560_PWR_ACTIVE_MUTED	0x41
#define TAS2560_PWR_ACTIVE		0x40
#define TAS2560_PWR_SHUTDOWN		0x01
#define TAS2560_SR_CTRL1		TAS2560_REG(0, 8)
#define TAS2560_LOAD			TAS2560_REG(0, 9)
#define TAS2560_LOAD_MASK		GENMASK(4, 3)
#define TAS2560_SR_CTRL2		TAS2560_REG(0, 13)
#define TAS2560_SR_CTRL3		TAS2560_REG(0, 14)
#define TAS2560_CLK_SEL			TAS2560_REG(0, 15)
#define TAS2560_PLL_SRC_MASK		GENMASK(7, 6)
#define TAS2560_PLL_SRC_BCLK		(0 << 6)
#define TAS2560_PLL_P_MASK		GENMASK(5, 0)
#define TAS2560_PLL_J			TAS2560_REG(0, 16)
#define TAS2560_PLL_J_LOW_FREQ		BIT(7)
#define TAS2560_PLL_D_LSB		TAS2560_REG(0, 17)
#define TAS2560_PLL_D_MSB		TAS2560_REG(0, 18)
#define TAS2560_DAI_FMT			TAS2560_REG(0, 20)
#define TAS2560_DAI_FMT_MASK		GENMASK(4, 2)
#define TAS2560_DAI_FMT_I2S		(0 << 2)
#define TAS2560_DAI_FMT_DSP		(1 << 2)
#define TAS2560_DAI_FMT_RIGHT_J		(2 << 2)
#define TAS2560_DAI_FMT_LEFT_J		(3 << 2)
#define TAS2560_WORD_LEN_MASK		GENMASK(1, 0)
#define TAS2560_ASI_CHANNEL		TAS2560_REG(0, 21)
#define TAS2560_ASI_CHANNEL_MASK	GENMASK(1, 0)
#define TAS2560_ASI_CFG_1		TAS2560_REG(0, 24)
#define TAS2560_ASI_CFG_1_MASK		GENMASK(5, 0)
#define TAS2560_BCLK_INV		BIT(2)
#define TAS2560_WCLK_INV		BIT(3)
#define TAS2560_CLK_ERR_CTRL		TAS2560_REG(0, 33)
#define TAS2560_INT_GEN			TAS2560_REG(0, 37)
#define TAS2560_DR_BOOST		TAS2560_REG(0, 73)
#define TAS2560_ID			TAS2560_REG(0, 125)

#define TAS2560_HPF_CUTOFF_CTL1		TAS2560_REG(50, 28)
#define TAS2560_ISENSE_PATH_CTL1	TAS2560_REG(50, 40)
#define TAS2560_BOOST_ON		TAS2560_REG(51, 16)
#define TAS2560_BOOST_HEAD		TAS2560_REG(51, 24)
#define TAS2560_THERMAL_FOLDBACK	TAS2560_REG(51, 100)
#define TAS2560_VSENSE_DEL_CTL1		TAS2560_REG(52, 52)

#define TAS2560_MAX_REG			TAS2560_REG(52, 127)

enum tas2560_load {
	TAS2560_LOAD_8OHM,
	TAS2560_LOAD_6OHM,
	TAS2560_LOAD_4OHM,
};

struct tas2560 {
	struct device *dev;
	struct regmap *regmap;
	struct gpio_desc *reset_gpio;
	enum tas2560_load load;
	u32 asi_channel;
	unsigned int rate;
	unsigned int frame_size;
};

/* A coefficient block: consecutive registers starting at @reg */
struct tas2560_block {
	unsigned int reg;
	u8 len;
	u8 data[4];
};

#define BLOCK4(r, a, b, c, d)	{ .reg = (r), .len = 4, .data = { a, b, c, d } }
#define BLOCK1(r, a)		{ .reg = (r), .len = 1, .data = { a } }

static const struct tas2560_block tas2560_hpf[] = {
	/* Isense path HPF cut off 2 Hz */
	BLOCK4(TAS2560_ISENSE_PATH_CTL1,	0x7f, 0xfb, 0xb5, 0x00),
	BLOCK4(TAS2560_ISENSE_PATH_CTL1 + 4,	0x80, 0x04, 0x4c, 0x00),
	BLOCK4(TAS2560_ISENSE_PATH_CTL1 + 8,	0x7f, 0xf7, 0x6a, 0x00),
	/* All pass */
	BLOCK4(TAS2560_HPF_CUTOFF_CTL1,		0x7f, 0xff, 0xff, 0xff),
	BLOCK4(TAS2560_HPF_CUTOFF_CTL1 + 4,	0x00, 0x00, 0x00, 0x00),
	BLOCK4(TAS2560_HPF_CUTOFF_CTL1 + 8,	0x00, 0x00, 0x00, 0x00),
};

static const struct tas2560_block tas2560_boost_headroom[] = {
	BLOCK4(TAS2560_BOOST_HEAD,		0x06, 0x66, 0x66, 0x00),
};

static const struct tas2560_block tas2560_thermal_foldback[] = {
	BLOCK4(TAS2560_THERMAL_FOLDBACK,	0x39, 0x80, 0x00, 0x00),
};

static const struct tas2560_block tas2560_vsense_biquad[] = {
	/* Vsense delay in biquad = 3/8 sample at 48 kHz */
	BLOCK4(TAS2560_VSENSE_DEL_CTL1,		0x3a, 0x46, 0x74, 0x00),
	BLOCK4(TAS2560_VSENSE_DEL_CTL1 + 4,	0x22, 0xf3, 0x07, 0x00),
	BLOCK4(TAS2560_VSENSE_DEL_CTL1 + 8,	0x80, 0x77, 0x61, 0x00),
	BLOCK4(TAS2560_VSENSE_DEL_CTL1 + 12,	0x22, 0xa7, 0xcc, 0x00),
	BLOCK4(TAS2560_VSENSE_DEL_CTL1 + 16,	0x3a, 0x0c, 0x93, 0x00),
};

/* Boost on/off thresholds and boost table, per speaker impedance */
static const struct tas2560_block tas2560_load_4ohm[] = {
	BLOCK4(TAS2560_BOOST_ON,		0x6f, 0x5c, 0x28, 0xf5),
	BLOCK4(TAS2560_BOOST_ON + 4,		0x67, 0xae, 0x14, 0x7a),
	BLOCK4(TAS2560_BOOST_ON + 16,		0x1c, 0x00, 0x00, 0x00),
	BLOCK4(TAS2560_BOOST_ON + 20,		0x1f, 0x0a, 0x3d, 0x70),
	BLOCK4(TAS2560_BOOST_ON + 24,		0x22, 0x14, 0x7a, 0xe1),
	BLOCK4(TAS2560_BOOST_ON + 28,		0x25, 0x1e, 0xb8, 0x51),
	BLOCK4(TAS2560_BOOST_ON + 32,		0x28, 0x28, 0xf5, 0xc2),
	BLOCK4(TAS2560_BOOST_ON + 36,		0x2b, 0x33, 0x33, 0x33),
	BLOCK4(TAS2560_BOOST_ON + 40,		0x2e, 0x3d, 0x70, 0xa3),
	BLOCK4(TAS2560_BOOST_ON + 44,		0x31, 0x47, 0xae, 0x14),
};

static const struct tas2560_block tas2560_load_6ohm[] = {
	BLOCK4(TAS2560_BOOST_ON,		0x73, 0x33, 0x33, 0x33),
	BLOCK4(TAS2560_BOOST_ON + 4,		0x6b, 0x85, 0x1e, 0xb8),
	BLOCK4(TAS2560_BOOST_ON + 16,		0x1d, 0x99, 0x99, 0x99),
	BLOCK4(TAS2560_BOOST_ON + 20,		0x20, 0xcc, 0xcc, 0xcc),
	BLOCK4(TAS2560_BOOST_ON + 24,		0x24, 0x00, 0x00, 0x00),
	BLOCK4(TAS2560_BOOST_ON + 28,		0x27, 0x33, 0x33, 0x33),
	BLOCK4(TAS2560_BOOST_ON + 32,		0x2a, 0x66, 0x66, 0x66),
	BLOCK4(TAS2560_BOOST_ON + 36,		0x2d, 0x99, 0x99, 0x99),
	BLOCK4(TAS2560_BOOST_ON + 40,		0x30, 0xcc, 0xcc, 0xcc),
	BLOCK4(TAS2560_BOOST_ON + 44,		0x34, 0x00, 0x00, 0x00),
};

static const struct tas2560_block tas2560_load_8ohm[] = {
	BLOCK4(TAS2560_BOOST_ON,		0x75, 0xc2, 0x8e, 0x00),
	BLOCK4(TAS2560_BOOST_ON + 4,		0x6e, 0x14, 0x79, 0x00),
	BLOCK4(TAS2560_BOOST_ON + 16,		0x1e, 0x00, 0x00, 0x00),
	BLOCK4(TAS2560_BOOST_ON + 20,		0x21, 0x3d, 0x71, 0x00),
	BLOCK4(TAS2560_BOOST_ON + 24,		0x24, 0x7a, 0xe1, 0x00),
	BLOCK4(TAS2560_BOOST_ON + 28,		0x27, 0xb8, 0x52, 0x00),
	BLOCK4(TAS2560_BOOST_ON + 32,		0x2a, 0xf5, 0xc3, 0x00),
	BLOCK4(TAS2560_BOOST_ON + 36,		0x2e, 0x33, 0x33, 0x00),
	BLOCK4(TAS2560_BOOST_ON + 40,		0x31, 0x70, 0xa4, 0x00),
	BLOCK4(TAS2560_BOOST_ON + 44,		0x34, 0xae, 0x14, 0x00),
};

static int tas2560_write_blocks(struct tas2560 *tas2560,
				const struct tas2560_block *blocks,
				unsigned int count)
{
	unsigned int i, j;
	int ret;

	for (i = 0; i < count; i++) {
		for (j = 0; j < blocks[i].len; j++) {
			ret = regmap_write(tas2560->regmap, blocks[i].reg + j,
					   blocks[i].data[j]);
			if (ret)
				return ret;
		}
	}

	return 0;
}

#define tas2560_write_table(t, table) \
	tas2560_write_blocks(t, table, ARRAY_SIZE(table))

static int tas2560_set_load(struct tas2560 *tas2560)
{
	int ret;

	switch (tas2560->load) {
	case TAS2560_LOAD_4OHM:
		ret = tas2560_write_table(tas2560, tas2560_load_4ohm);
		break;
	case TAS2560_LOAD_6OHM:
		ret = tas2560_write_table(tas2560, tas2560_load_6ohm);
		break;
	default:
		ret = tas2560_write_table(tas2560, tas2560_load_8ohm);
		break;
	}
	if (ret)
		return ret;

	return regmap_update_bits(tas2560->regmap, TAS2560_LOAD,
				  TAS2560_LOAD_MASK,
				  FIELD_PREP(TAS2560_LOAD_MASK, tas2560->load));
}

static int tas2560_set_rate(struct tas2560 *tas2560, unsigned int rate)
{
	u8 sr1, sr2, sr3;
	int ret;

	switch (rate) {
	case 48000:
		sr1 = 0x01; sr2 = 0x08; sr3 = 0x10;
		break;
	case 44100:
		sr1 = 0x11; sr2 = 0x08; sr3 = 0x10;
		break;
	case 16000:
		sr1 = 0x01; sr2 = 0x18; sr3 = 0x20;
		break;
	case 8000:
		sr1 = 0x01; sr2 = 0x30; sr3 = 0x20;
		break;
	default:
		dev_err(tas2560->dev, "unsupported sample rate %u\n", rate);
		return -EINVAL;
	}

	ret = regmap_write(tas2560->regmap, TAS2560_SR_CTRL1, sr1);
	if (ret)
		return ret;

	ret = regmap_write(tas2560->regmap, TAS2560_SR_CTRL2, sr2);
	if (ret)
		return ret;

	return regmap_write(tas2560->regmap, TAS2560_SR_CTRL3, sr3);
}

/* PLL output must be rate * 1024: pll_clk = pll_in * J.D / P */
static int tas2560_set_pll(struct tas2560 *tas2560, unsigned int pll_in)
{
	unsigned int pll_clk = tas2560->rate * 1024;
	unsigned int p, j, d, div_in;
	int ret;

	if (pll_in <= 40000000)
		p = 1;
	else if (pll_in <= 80000000)
		p = 2;
	else if (pll_in <= 160000000)
		p = 3;
	else
		return -EINVAL;

	j = pll_clk * p / pll_in;
	d = (pll_clk * p % pll_in) / (pll_in / 10000);
	div_in = pll_in / (1 << p);

	if (!j || (!d && (div_in < 512000 || div_in > 20000000)) ||
	    (d && (div_in < 10000000 || div_in > 20000000))) {
		dev_err(tas2560->dev, "no PLL setting for %u Hz input\n",
			pll_in);
		return -EINVAL;
	}

	dev_dbg(tas2560->dev, "PLL in %u Hz, P=%u J.D=%u.%04u\n",
		pll_in, p, j, d);

	ret = regmap_update_bits(tas2560->regmap, TAS2560_CLK_SEL,
				 TAS2560_PLL_SRC_MASK | TAS2560_PLL_P_MASK,
				 TAS2560_PLL_SRC_BCLK | p);
	if (ret)
		return ret;

	ret = regmap_write(tas2560->regmap, TAS2560_PLL_J,
			   j | (pll_in < 1000000 ? TAS2560_PLL_J_LOW_FREQ : 0));
	if (ret)
		return ret;

	ret = regmap_write(tas2560->regmap, TAS2560_PLL_D_LSB, d & 0xff);
	if (ret)
		return ret;

	return regmap_write(tas2560->regmap, TAS2560_PLL_D_MSB, (d >> 8) & 0x3f);
}

static int tas2560_power_up(struct tas2560 *tas2560)
{
	int ret;

	ret = regmap_write(tas2560->regmap, TAS2560_PWR,
			   TAS2560_PWR_ACTIVE_MUTED);
	if (ret)
		return ret;

	usleep_range(10000, 11000);

	ret = tas2560_write_table(tas2560, tas2560_hpf);
	if (!ret)
		ret = tas2560_set_load(tas2560);
	if (!ret)
		ret = tas2560_write_table(tas2560, tas2560_boost_headroom);
	if (!ret)
		ret = tas2560_write_table(tas2560, tas2560_thermal_foldback);
	if (!ret)
		ret = tas2560_write_table(tas2560, tas2560_vsense_biquad);
	if (!ret)
		ret = tas2560_set_rate(tas2560, tas2560->rate);
	if (!ret)
		ret = regmap_write(tas2560->regmap, TAS2560_CLK_ERR_CTRL, 0x0b);
	if (!ret)
		ret = regmap_write(tas2560->regmap, TAS2560_INT_GEN, 0xff);
	if (!ret)
		ret = regmap_write(tas2560->regmap, TAS2560_PWR,
				   TAS2560_PWR_ACTIVE);

	return ret;
}

static int tas2560_power_down(struct tas2560 *tas2560)
{
	int ret;

	ret = regmap_write(tas2560->regmap, TAS2560_INT_GEN, 0x00);
	if (!ret)
		ret = regmap_write(tas2560->regmap, TAS2560_CLK_ERR_CTRL, 0x00);
	if (!ret)
		ret = regmap_write(tas2560->regmap, TAS2560_PWR,
				   TAS2560_PWR_ACTIVE_MUTED);
	if (ret)
		return ret;

	msleep(30);

	ret = regmap_write(tas2560->regmap, TAS2560_PWR, TAS2560_PWR_SHUTDOWN);
	usleep_range(10000, 11000);

	return ret;
}

static int tas2560_classd_event(struct snd_soc_dapm_widget *w,
				struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component =
		snd_soc_dapm_to_component(w->dapm);
	struct tas2560 *tas2560 = snd_soc_component_get_drvdata(component);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		return tas2560_power_up(tas2560);
	case SND_SOC_DAPM_PRE_PMD:
		return tas2560_power_down(tas2560);
	}

	return 0;
}

static const struct snd_soc_dapm_widget tas2560_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN("ASI1", "ASI1 Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_OUT_DRV_E("ClassD", SND_SOC_NOPM, 0, 0, NULL, 0,
			       tas2560_classd_event,
			       SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_OUTPUT("OUT"),
	SND_SOC_DAPM_AIF_OUT("ASI1 OUT", "ASI1 Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_INPUT("IV"),
};

static const struct snd_soc_dapm_route tas2560_dapm_routes[] = {
	{ "ClassD", NULL, "ASI1" },
	{ "OUT", NULL, "ClassD" },
	{ "ASI1 OUT", NULL, "IV" },
};

/* Class-D gain, 0..15 dB in 1 dB steps */
static const DECLARE_TLV_DB_SCALE(tas2560_gain_tlv, 0, 100, 0);

static const char * const tas2560_asi_channel_text[] = {
	"Left", "Right", "Left+Right", "Mono",
};

static SOC_ENUM_SINGLE_DECL(tas2560_asi_channel_enum, TAS2560_ASI_CHANNEL,
			    0, tas2560_asi_channel_text);

static const struct snd_kcontrol_new tas2560_controls[] = {
	SOC_SINGLE_TLV("Amp Gain Volume", TAS2560_SPK_CTRL, 0, 15, 0,
		       tas2560_gain_tlv),
	SOC_ENUM("ASI Channel", tas2560_asi_channel_enum),
};

static int tas2560_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct tas2560 *tas2560 = snd_soc_component_get_drvdata(component);
	unsigned int word_len;
	int ret;

	switch (params_width(params)) {
	case 16:
		word_len = 0;
		break;
	case 20:
		word_len = 1;
		break;
	case 24:
		word_len = 2;
		break;
	case 32:
		word_len = 3;
		break;
	default:
		return -EINVAL;
	}

	ret = regmap_update_bits(tas2560->regmap, TAS2560_DAI_FMT,
				 TAS2560_WORD_LEN_MASK, word_len);
	if (ret)
		return ret;

	tas2560->rate = params_rate(params);
	tas2560->frame_size = snd_soc_params_to_frame_size(params);

	ret = tas2560_set_rate(tas2560, tas2560->rate);
	if (ret)
		return ret;

	return tas2560_set_pll(tas2560, tas2560->rate * tas2560->frame_size);
}

static int tas2560_set_dai_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_component *component = dai->component;
	struct tas2560 *tas2560 = snd_soc_component_get_drvdata(component);
	unsigned int asi_cfg = 0, dai_fmt;
	int ret;

	/* The amplifier only takes BCLK and WCLK from the host */
	if ((fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_BC_FC)
		return -EINVAL;

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		break;
	case SND_SOC_DAIFMT_NB_IF:
		asi_cfg = TAS2560_WCLK_INV;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		asi_cfg = TAS2560_BCLK_INV;
		break;
	case SND_SOC_DAIFMT_IB_IF:
		asi_cfg = TAS2560_WCLK_INV | TAS2560_BCLK_INV;
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		dai_fmt = TAS2560_DAI_FMT_I2S;
		break;
	case SND_SOC_DAIFMT_DSP_A:
	case SND_SOC_DAIFMT_DSP_B:
		dai_fmt = TAS2560_DAI_FMT_DSP;
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		dai_fmt = TAS2560_DAI_FMT_RIGHT_J;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		dai_fmt = TAS2560_DAI_FMT_LEFT_J;
		break;
	default:
		return -EINVAL;
	}

	ret = regmap_update_bits(tas2560->regmap, TAS2560_ASI_CFG_1,
				 TAS2560_ASI_CFG_1_MASK, asi_cfg);
	if (ret)
		return ret;

	return regmap_update_bits(tas2560->regmap, TAS2560_DAI_FMT,
				  TAS2560_DAI_FMT_MASK, dai_fmt);
}

static const struct snd_soc_dai_ops tas2560_dai_ops = {
	.hw_params = tas2560_hw_params,
	.set_fmt = tas2560_set_dai_fmt,
};

#define TAS2560_RATES	(SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 | \
			 SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000)
#define TAS2560_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE | \
			 SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver tas2560_dai = {
	.name = "tas2560-amplifier",
	.playback = {
		.stream_name = "ASI1 Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = TAS2560_RATES,
		.formats = TAS2560_FORMATS,
	},
	/* Current/voltage sense data for speaker protection */
	.capture = {
		.stream_name = "ASI1 Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rates = TAS2560_RATES,
		.formats = TAS2560_FORMATS,
	},
	.ops = &tas2560_dai_ops,
};

static int tas2560_component_probe(struct snd_soc_component *component)
{
	struct tas2560 *tas2560 = snd_soc_component_get_drvdata(component);

	return regmap_update_bits(tas2560->regmap, TAS2560_ASI_CHANNEL,
				  TAS2560_ASI_CHANNEL_MASK,
				  tas2560->asi_channel);
}

static const struct snd_soc_component_driver tas2560_component = {
	.probe = tas2560_component_probe,
	.controls = tas2560_controls,
	.num_controls = ARRAY_SIZE(tas2560_controls),
	.dapm_widgets = tas2560_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(tas2560_dapm_widgets),
	.dapm_routes = tas2560_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(tas2560_dapm_routes),
	.endianness = 1,
};

static const struct regmap_range_cfg tas2560_ranges[] = {
	{
		.range_min = 0,
		.range_max = TAS2560_MAX_REG,
		.selector_reg = TAS2560_PAGE_SEL,
		.selector_mask = 0xff,
		.selector_shift = 0,
		.window_start = 0,
		.window_len = 128,
	},
};

static const struct regmap_config tas2560_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = TAS2560_MAX_REG,
	.ranges = tas2560_ranges,
	.num_ranges = ARRAY_SIZE(tas2560_ranges),
	.cache_type = REGCACHE_NONE,
};

static int tas2560_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct tas2560 *tas2560;
	u32 load_ohms = 8;
	unsigned int id;
	int ret;

	tas2560 = devm_kzalloc(dev, sizeof(*tas2560), GFP_KERNEL);
	if (!tas2560)
		return -ENOMEM;

	tas2560->dev = dev;
	tas2560->rate = 48000;
	i2c_set_clientdata(client, tas2560);

	tas2560->regmap = devm_regmap_init_i2c(client, &tas2560_regmap);
	if (IS_ERR(tas2560->regmap))
		return dev_err_probe(dev, PTR_ERR(tas2560->regmap),
				     "failed to init regmap\n");

	device_property_read_u32(dev, "ti,load-ohms", &load_ohms);
	switch (load_ohms) {
	case 4:
		tas2560->load = TAS2560_LOAD_4OHM;
		break;
	case 6:
		tas2560->load = TAS2560_LOAD_6OHM;
		break;
	case 8:
		tas2560->load = TAS2560_LOAD_8OHM;
		break;
	default:
		return dev_err_probe(dev, -EINVAL,
				     "unsupported load %u ohms\n", load_ohms);
	}

	/* Default: average of both I2S channels */
	tas2560->asi_channel = 2;
	device_property_read_u32(dev, "ti,asi-channel", &tas2560->asi_channel);
	if (tas2560->asi_channel > 3)
		return dev_err_probe(dev, -EINVAL, "invalid ti,asi-channel\n");

	/* Asserted (in reset) until the chip is set up */
	tas2560->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						      GPIOD_OUT_HIGH);
	if (IS_ERR(tas2560->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(tas2560->reset_gpio),
				     "failed to get reset GPIO\n");

	if (tas2560->reset_gpio) {
		usleep_range(5000, 6000);
		gpiod_set_value_cansleep(tas2560->reset_gpio, 0);
		usleep_range(1000, 2000);
	}

	ret = regmap_write(tas2560->regmap, TAS2560_SW_RESET, 0x01);
	if (ret)
		return dev_err_probe(dev, ret, "software reset failed\n");

	usleep_range(1000, 2000);

	ret = regmap_read(tas2560->regmap, TAS2560_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read ID\n");

	dev_info(dev, "TAS2560 found, ID register 0x%02x\n", id);

	ret = regmap_write(tas2560->regmap, TAS2560_DR_BOOST, 0x04);
	if (!ret)
		ret = regmap_write(tas2560->regmap, TAS2560_DEV_MODE, 0x02);
	if (!ret)
		ret = regmap_write(tas2560->regmap, TAS2560_PWR,
				   TAS2560_PWR_SHUTDOWN);
	if (ret)
		return dev_err_probe(dev, ret, "failed to init device\n");

	return devm_snd_soc_register_component(dev, &tas2560_component,
					       &tas2560_dai, 1);
}

static const struct of_device_id tas2560_of_match[] = {
	{ .compatible = "ti,tas2560" },
	{ }
};
MODULE_DEVICE_TABLE(of, tas2560_of_match);

static const struct i2c_device_id tas2560_i2c_id[] = {
	{ "tas2560" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tas2560_i2c_id);

static struct i2c_driver tas2560_i2c_driver = {
	.driver = {
		.name = "tas2560",
		.of_match_table = tas2560_of_match,
	},
	.probe = tas2560_probe,
	.id_table = tas2560_i2c_id,
};
module_i2c_driver(tas2560_i2c_driver);

MODULE_DESCRIPTION("ASoC TAS2560 smart amplifier driver");
MODULE_LICENSE("GPL");
