// SPDX-License-Identifier: GPL-2.0
/*
 * Sony IMX386 image sensor driver
 *
 * Register settings were taken from the Xiaomi Mi Max 2 (oxygen) vendor
 * sensor library (libmmcamera_oxygen_imx386_sunny.so).
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/units.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#define IMX386_MCLK_FREQ_24MHZ		(24 * HZ_PER_MHZ)
#define IMX386_DATA_LANES		4

#define IMX386_REG_CHIP_ID		CCI_REG16(0x0016)
#define IMX386_CHIP_ID			0x0386

#define IMX386_REG_MODE_SELECT		CCI_REG8(0x0100)
#define IMX386_MODE_STANDBY		0x00
#define IMX386_MODE_STREAMING		0x01

#define IMX386_REG_ORIENTATION		CCI_REG8(0x0101)
#define IMX386_HFLIP			BIT(0)
#define IMX386_VFLIP			BIT(1)

#define IMX386_REG_HOLD			CCI_REG8(0x0104)

#define IMX386_REG_EXPOSURE		CCI_REG16(0x0202)
#define IMX386_EXPOSURE_MIN		1
#define IMX386_EXPOSURE_STEP		1
#define IMX386_EXPOSURE_DEFAULT		1000
#define IMX386_EXPOSURE_MARGIN		10

/* Analogue gain = 512 / (512 - code), 1x..16x */
#define IMX386_REG_AGAIN		CCI_REG16(0x0204)
#define IMX386_AGAIN_MIN		0
#define IMX386_AGAIN_MAX		480
#define IMX386_AGAIN_STEP		1
#define IMX386_AGAIN_DEFAULT		0

/* Digital gain, 0x100 = 1x */
#define IMX386_REG_DGAIN		CCI_REG16(0x020e)
#define IMX386_DGAIN_MIN		0x100
#define IMX386_DGAIN_MAX		0xfff
#define IMX386_DGAIN_STEP		1
#define IMX386_DGAIN_DEFAULT		0x100

#define IMX386_REG_VTS			CCI_REG16(0x0340)
#define IMX386_VTS_MAX			0xffff

#define IMX386_REG_TEST_PATTERN		CCI_REG16(0x0600)

#define IMX386_NATIVE_WIDTH		4032
#define IMX386_NATIVE_HEIGHT		3016

#define to_imx386(_sd)			container_of(_sd, struct imx386, sd)

#include "tables.h"

enum {
	IMX386_LINK_FREQ_498MHZ,
	IMX386_LINK_FREQ_172MHZ,
	IMX386_LINK_FREQ_385MHZ,
	IMX386_LINK_FREQ_446MHZ,
};

static const s64 imx386_link_freq_menu[] = {
	[IMX386_LINK_FREQ_498MHZ] = 498 * HZ_PER_MHZ,
	[IMX386_LINK_FREQ_172MHZ] = 172 * HZ_PER_MHZ,
	[IMX386_LINK_FREQ_385MHZ] = 385 * HZ_PER_MHZ,
	[IMX386_LINK_FREQ_446MHZ] = 446 * HZ_PER_MHZ,
};

/* Indexed by the ORIENTATION register flip bits */
static const u32 imx386_mbus_formats[] = {
	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SGRBG10_1X10,
	MEDIA_BUS_FMT_SGBRG10_1X10,
	MEDIA_BUS_FMT_SBGGR10_1X10,
};

struct imx386_mode {
	u32 width;
	u32 height;
	u32 hts;
	u32 vts;
	u64 pixel_rate;
	unsigned int link_freq_index;
	struct v4l2_rect crop;
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

#define IMX386_MODE_REGS(_regs) .regs = _regs, .num_regs = ARRAY_SIZE(_regs)

static const struct imx386_mode imx386_modes[] = {
	{
		/* Full resolution, 29.4 fps */
		.width = 4032,
		.height = 3016,
		.hts = 4296,
		.vts = 3070,
		.pixel_rate = 388000000,
		.link_freq_index = IMX386_LINK_FREQ_498MHZ,
		.crop = { 0, 0, 4032, 3016 },
		IMX386_MODE_REGS(imx386_mode0_regs),
	},
	{
		/* 16:9 crop, 30 fps */
		.width = 4032,
		.height = 2256,
		.hts = 4296,
		.vts = 2310,
		.pixel_rate = 298000000,
		.link_freq_index = IMX386_LINK_FREQ_385MHZ,
		.crop = { 0, 380, 4032, 2256 },
		IMX386_MODE_REGS(imx386_mode2_regs),
	},
	{
		/* 4K UHD crop, 30 fps */
		.width = 3840,
		.height = 2160,
		.hts = 4296,
		.vts = 2306,
		.pixel_rate = 297333333,
		.link_freq_index = IMX386_LINK_FREQ_446MHZ,
		.crop = { 96, 428, 3840, 2160 },
		IMX386_MODE_REGS(imx386_mode3_regs),
	},
	{
		/* 2x2 binned, 30 fps */
		.width = 2016,
		.height = 1508,
		.hts = 2256,
		.vts = 1692,
		.pixel_rate = 114666667,
		.link_freq_index = IMX386_LINK_FREQ_172MHZ,
		.crop = { 0, 0, 4032, 3016 },
		IMX386_MODE_REGS(imx386_mode1_regs),
	},
	{
		/* 2x2 binned 1080p, 30 fps */
		.width = 1920,
		.height = 1080,
		.hts = 2256,
		.vts = 1692,
		.pixel_rate = 114666667,
		.link_freq_index = IMX386_LINK_FREQ_172MHZ,
		.crop = { 96, 428, 3840, 2160 },
		IMX386_MODE_REGS(imx386_mode4_regs),
	},
};

static const char * const imx386_test_pattern_menu[] = {
	"Disabled",
	"Solid colour",
	"Colour bars",
	"Fade to grey colour bars",
	"PN9",
};

/* In power-up order */
static const char * const imx386_supply_names[] = {
	"avdd",		/* Analog power, 2.8 V */
	"dvdd",		/* Digital core power, 1.1 V */
	"dovdd",	/* Digital I/O power, 1.8 V */
};

#define IMX386_NUM_SUPPLIES	ARRAY_SIZE(imx386_supply_names)

struct imx386 {
	struct device *dev;
	struct regmap *regmap;
	struct clk *mclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[IMX386_NUM_SUPPLIES];
	unsigned long link_freq_bitmap;

	struct v4l2_subdev sd;
	struct media_pad pad;

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vflip;
	struct v4l2_ctrl *hflip;

	const struct imx386_mode *mode;
};

static bool imx386_mode_supported(struct imx386 *imx386,
				  const struct imx386_mode *mode)
{
	return imx386->link_freq_bitmap & BIT(mode->link_freq_index);
}

static u32 imx386_get_format_code(struct imx386 *imx386);

static int imx386_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx386 *imx386 = container_of(ctrl->handler, struct imx386,
					     ctrl_handler);
	const struct imx386_mode *mode = imx386->mode;
	struct v4l2_subdev_state *state;
	s64 exposure_max;
	int ret;

	switch (ctrl->id) {
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		/* Flipping changes the Bayer order of the active format */
		if (imx386->sd.active_state) {
			state = v4l2_subdev_get_locked_active_state(&imx386->sd);
			v4l2_subdev_state_get_format(state, 0)->code =
				imx386_get_format_code(imx386);
		}
		break;
	case V4L2_CID_VBLANK:
		exposure_max = mode->height + ctrl->val - IMX386_EXPOSURE_MARGIN;
		__v4l2_ctrl_modify_range(imx386->exposure,
					 imx386->exposure->minimum,
					 exposure_max,
					 imx386->exposure->step,
					 min(imx386->exposure->default_value,
					     exposure_max));
		break;
	}

	/* V4L2 controls are applied when the sensor is powered up */
	if (!pm_runtime_get_if_active(imx386->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(imx386->regmap, IMX386_REG_AGAIN,
				ctrl->val, NULL);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		ret = cci_write(imx386->regmap, IMX386_REG_DGAIN,
				ctrl->val, NULL);
		break;
	case V4L2_CID_EXPOSURE:
		ret = cci_write(imx386->regmap, IMX386_REG_EXPOSURE,
				ctrl->val, NULL);
		break;
	case V4L2_CID_VBLANK:
		ret = cci_write(imx386->regmap, IMX386_REG_VTS,
				mode->height + ctrl->val, NULL);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = cci_write(imx386->regmap, IMX386_REG_TEST_PATTERN,
				ctrl->val, NULL);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		ret = cci_write(imx386->regmap, IMX386_REG_ORIENTATION,
				(imx386->hflip->val ? IMX386_HFLIP : 0) |
				(imx386->vflip->val ? IMX386_VFLIP : 0),
				NULL);
		break;
	case V4L2_CID_LINK_FREQ:
	case V4L2_CID_PIXEL_RATE:
	case V4L2_CID_HBLANK:
		/* Fixed by the mode, written with the mode registers */
		ret = 0;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(imx386->dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx386_ctrl_ops = {
	.s_ctrl = imx386_set_ctrl,
};

static int imx386_init_controls(struct imx386 *imx386)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &imx386->ctrl_handler;
	const struct imx386_mode *mode = imx386->mode;
	struct v4l2_fwnode_device_properties props;
	s64 hblank, vblank, exposure_max;
	int ret;

	v4l2_ctrl_handler_init(ctrl_hdlr, 12);

	imx386->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, &imx386_ctrl_ops,
					V4L2_CID_LINK_FREQ,
					ARRAY_SIZE(imx386_link_freq_menu) - 1,
					mode->link_freq_index,
					imx386_link_freq_menu);
	if (imx386->link_freq)
		imx386->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx386->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &imx386_ctrl_ops,
					       V4L2_CID_PIXEL_RATE,
					       mode->pixel_rate,
					       mode->pixel_rate, 1,
					       mode->pixel_rate);

	hblank = mode->hts - mode->width;
	imx386->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx386_ctrl_ops,
					   V4L2_CID_HBLANK, hblank,
					   hblank, 1, hblank);
	if (imx386->hblank)
		imx386->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank = mode->vts - mode->height;
	imx386->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx386_ctrl_ops,
					   V4L2_CID_VBLANK, vblank,
					   IMX386_VTS_MAX - mode->height, 1,
					   vblank);

	v4l2_ctrl_new_std(ctrl_hdlr, &imx386_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  IMX386_AGAIN_MIN, IMX386_AGAIN_MAX,
			  IMX386_AGAIN_STEP, IMX386_AGAIN_DEFAULT);

	v4l2_ctrl_new_std(ctrl_hdlr, &imx386_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
			  IMX386_DGAIN_MIN, IMX386_DGAIN_MAX,
			  IMX386_DGAIN_STEP, IMX386_DGAIN_DEFAULT);

	exposure_max = mode->vts - IMX386_EXPOSURE_MARGIN;
	imx386->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &imx386_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX386_EXPOSURE_MIN,
					     exposure_max,
					     IMX386_EXPOSURE_STEP,
					     IMX386_EXPOSURE_DEFAULT);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &imx386_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(imx386_test_pattern_menu) - 1,
				     0, 0, imx386_test_pattern_menu);

	imx386->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx386_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (imx386->hflip)
		imx386->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	imx386->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx386_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (imx386->vflip)
		imx386->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		goto error_free_hdlr;
	}

	ret = v4l2_fwnode_device_parse(imx386->dev, &props);
	if (ret)
		goto error_free_hdlr;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &imx386_ctrl_ops,
					      &props);
	if (ret)
		goto error_free_hdlr;

	imx386->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error_free_hdlr:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static int imx386_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct imx386 *imx386 = to_imx386(sd);
	const struct imx386_mode *mode = imx386->mode;
	int ret;

	ret = pm_runtime_resume_and_get(imx386->dev);
	if (ret)
		return ret;

	cci_multi_reg_write(imx386->regmap, imx386_init_regs,
			    ARRAY_SIZE(imx386_init_regs), &ret);
	cci_multi_reg_write(imx386->regmap, mode->regs, mode->num_regs, &ret);
	if (ret)
		goto error;

	/* Mode tables reset orientation, controls restore it */
	ret = __v4l2_ctrl_handler_setup(imx386->sd.ctrl_handler);
	if (ret)
		goto error;

	ret = cci_write(imx386->regmap, IMX386_REG_MODE_SELECT,
			IMX386_MODE_STREAMING, NULL);
	if (ret)
		goto error;

	/* Flipping changes the Bayer order, don't allow it while streaming */
	__v4l2_ctrl_grab(imx386->vflip, true);
	__v4l2_ctrl_grab(imx386->hflip, true);

	return 0;

error:
	dev_err(imx386->dev, "failed to start streaming: %d\n", ret);
	pm_runtime_put_autosuspend(imx386->dev);

	return ret;
}

static int imx386_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct imx386 *imx386 = to_imx386(sd);
	int ret;

	ret = cci_write(imx386->regmap, IMX386_REG_MODE_SELECT,
			IMX386_MODE_STANDBY, NULL);
	if (ret)
		dev_err(imx386->dev, "failed to stop streaming: %d\n", ret);

	__v4l2_ctrl_grab(imx386->vflip, false);
	__v4l2_ctrl_grab(imx386->hflip, false);

	pm_runtime_put_autosuspend(imx386->dev);

	return ret;
}

static u32 imx386_get_format_code(struct imx386 *imx386)
{
	unsigned int i;

	i = (imx386->vflip->val ? IMX386_VFLIP : 0) |
	    (imx386->hflip->val ? IMX386_HFLIP : 0);

	return imx386_mbus_formats[i];
}

static void imx386_update_pad_format(struct imx386 *imx386,
				     const struct imx386_mode *mode,
				     struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = imx386_get_format_code(imx386);
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static const struct imx386_mode *imx386_find_mode(struct imx386 *imx386,
						  u32 width, u32 height)
{
	const struct imx386_mode *best = NULL;
	u32 best_dist = U32_MAX;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(imx386_modes); i++) {
		const struct imx386_mode *mode = &imx386_modes[i];
		u32 dist;

		if (!imx386_mode_supported(imx386, mode))
			continue;

		dist = abs((s32)mode->width - (s32)width) +
		       abs((s32)mode->height - (s32)height);
		if (dist < best_dist) {
			best = mode;
			best_dist = dist;
		}
	}

	return best;
}

static int imx386_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx386 *imx386 = to_imx386(sd);
	s64 hblank, vblank, exposure_max;
	const struct imx386_mode *mode;
	int ret;

	mode = imx386_find_mode(imx386, fmt->format.width, fmt->format.height);

	imx386_update_pad_format(imx386, mode, &fmt->format);
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;
	*v4l2_subdev_state_get_crop(state, 0) = mode->crop;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY || imx386->mode == mode)
		return 0;

	imx386->mode = mode;

	ret = __v4l2_ctrl_s_ctrl(imx386->link_freq, mode->link_freq_index);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_modify_range(imx386->pixel_rate, mode->pixel_rate,
				       mode->pixel_rate, 1, mode->pixel_rate);
	if (ret)
		return ret;

	hblank = mode->hts - mode->width;
	ret = __v4l2_ctrl_modify_range(imx386->hblank, hblank, hblank, 1,
				       hblank);
	if (ret)
		return ret;

	vblank = mode->vts - mode->height;
	ret = __v4l2_ctrl_modify_range(imx386->vblank, vblank,
				       IMX386_VTS_MAX - mode->height, 1,
				       vblank);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_s_ctrl(imx386->vblank, vblank);
	if (ret)
		return ret;

	exposure_max = mode->vts - IMX386_EXPOSURE_MARGIN;
	return __v4l2_ctrl_modify_range(imx386->exposure, IMX386_EXPOSURE_MIN,
					exposure_max, IMX386_EXPOSURE_STEP,
					min_t(s64, IMX386_EXPOSURE_DEFAULT,
					      exposure_max));
}

static int imx386_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx386 *imx386 = to_imx386(sd);

	if (code->index > 0)
		return -EINVAL;

	code->code = imx386_get_format_code(imx386);

	return 0;
}

static int imx386_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx386 *imx386 = to_imx386(sd);
	unsigned int i, index = 0;

	if (fse->code != imx386_get_format_code(imx386))
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(imx386_modes); i++) {
		if (!imx386_mode_supported(imx386, &imx386_modes[i]))
			continue;

		if (index++ == fse->index) {
			fse->min_width = imx386_modes[i].width;
			fse->max_width = fse->min_width;
			fse->min_height = imx386_modes[i].height;
			fse->max_height = fse->min_height;
			return 0;
		}
	}

	return -EINVAL;
}

static int imx386_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(sd_state, 0);
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = IMX386_NATIVE_WIDTH;
		sel->r.height = IMX386_NATIVE_HEIGHT;
		return 0;
	default:
		return -EINVAL;
	}
}

static int imx386_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct imx386 *imx386 = to_imx386(sd);
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.width = imx386->mode->width,
			.height = imx386->mode->height,
		},
	};

	return imx386_set_pad_format(sd, state, &fmt);
}

static const struct v4l2_subdev_video_ops imx386_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops imx386_pad_ops = {
	.set_fmt = imx386_set_pad_format,
	.get_fmt = v4l2_subdev_get_fmt,
	.get_selection = imx386_get_selection,
	.enum_mbus_code = imx386_enum_mbus_code,
	.enum_frame_size = imx386_enum_frame_size,
	.enable_streams = imx386_enable_streams,
	.disable_streams = imx386_disable_streams,
};

static const struct v4l2_subdev_ops imx386_subdev_ops = {
	.video = &imx386_video_ops,
	.pad = &imx386_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx386_internal_ops = {
	.init_state = imx386_init_state,
};

static const struct media_entity_operations imx386_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int imx386_identify_sensor(struct imx386 *imx386)
{
	u64 val;
	int ret;

	ret = cci_read(imx386->regmap, IMX386_REG_CHIP_ID, &val, NULL);
	if (ret)
		return dev_err_probe(imx386->dev, ret,
				     "failed to read chip id\n");

	if (val != IMX386_CHIP_ID)
		return dev_err_probe(imx386->dev, -ENODEV,
				     "chip id mismatch: %x!=%llx\n",
				     IMX386_CHIP_ID, val);

	return 0;
}

static int imx386_check_hwcfg(struct imx386 *imx386)
{
	struct fwnode_handle *fwnode = dev_fwnode(imx386->dev), *ep;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	int ret;

	if (!fwnode)
		return -ENODEV;

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep)
		return -EINVAL;

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != IMX386_DATA_LANES) {
		ret = dev_err_probe(imx386->dev, -EINVAL,
				    "invalid number of data lanes: %u\n",
				    bus_cfg.bus.mipi_csi2.num_data_lanes);
		goto endpoint_free;
	}

	ret = v4l2_link_freq_to_bitmap(imx386->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       imx386_link_freq_menu,
				       ARRAY_SIZE(imx386_link_freq_menu),
				       &imx386->link_freq_bitmap);

endpoint_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int imx386_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx386 *imx386 = to_imx386(sd);
	unsigned int i;
	int ret;

	gpiod_set_value_cansleep(imx386->reset_gpio, 1);

	/* Vendor sequence: VANA, VDIG, VIO, 1 ms apart */
	for (i = 0; i < IMX386_NUM_SUPPLIES; i++) {
		ret = regulator_enable(imx386->supplies[i].consumer);
		if (ret)
			goto disable_regulators;
		usleep_range(1000, 1100);
	}

	ret = clk_prepare_enable(imx386->mclk);
	if (ret)
		goto disable_regulators;

	usleep_range(1000, 1100);
	gpiod_set_value_cansleep(imx386->reset_gpio, 0);
	usleep_range(11 * USEC_PER_MSEC, 12 * USEC_PER_MSEC);

	return 0;

disable_regulators:
	while (i--)
		regulator_disable(imx386->supplies[i].consumer);

	return ret;
}

static int imx386_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx386 *imx386 = to_imx386(sd);
	unsigned int i;

	gpiod_set_value_cansleep(imx386->reset_gpio, 1);

	clk_disable_unprepare(imx386->mclk);

	for (i = IMX386_NUM_SUPPLIES; i--;)
		regulator_disable(imx386->supplies[i].consumer);

	return 0;
}

static int imx386_probe(struct i2c_client *client)
{
	struct imx386 *imx386;
	unsigned long freq;
	unsigned int i;
	int ret;

	imx386 = devm_kzalloc(&client->dev, sizeof(*imx386), GFP_KERNEL);
	if (!imx386)
		return -ENOMEM;

	imx386->dev = &client->dev;
	v4l2_i2c_subdev_init(&imx386->sd, client, &imx386_subdev_ops);

	imx386->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(imx386->regmap))
		return dev_err_probe(imx386->dev, PTR_ERR(imx386->regmap),
				     "failed to init CCI\n");

	imx386->mclk = devm_v4l2_sensor_clk_get(imx386->dev, NULL);
	if (IS_ERR(imx386->mclk))
		return dev_err_probe(imx386->dev, PTR_ERR(imx386->mclk),
				     "failed to get MCLK clock\n");

	freq = clk_get_rate(imx386->mclk);
	if (freq != IMX386_MCLK_FREQ_24MHZ)
		return dev_err_probe(imx386->dev, -EINVAL,
				     "MCLK clock frequency %lu is not supported\n",
				     freq);

	ret = imx386_check_hwcfg(imx386);
	if (ret)
		return dev_err_probe(imx386->dev, ret,
				     "failed to check HW configuration\n");

	imx386->reset_gpio = devm_gpiod_get_optional(imx386->dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(imx386->reset_gpio))
		return dev_err_probe(imx386->dev, PTR_ERR(imx386->reset_gpio),
				     "cannot get reset GPIO\n");

	for (i = 0; i < IMX386_NUM_SUPPLIES; i++)
		imx386->supplies[i].supply = imx386_supply_names[i];

	ret = devm_regulator_bulk_get(imx386->dev, IMX386_NUM_SUPPLIES,
				      imx386->supplies);
	if (ret)
		return dev_err_probe(imx386->dev, ret,
				     "failed to get supply regulators\n");

	/* The sensor must be powered on to read the CHIP_ID register */
	ret = imx386_power_on(imx386->dev);
	if (ret)
		return ret;

	ret = imx386_identify_sensor(imx386);
	if (ret)
		goto power_off;

	imx386->mode = imx386_find_mode(imx386, IMX386_NATIVE_WIDTH,
					IMX386_NATIVE_HEIGHT);
	ret = imx386_init_controls(imx386);
	if (ret) {
		dev_err_probe(imx386->dev, ret, "failed to init controls\n");
		goto power_off;
	}

	imx386->sd.state_lock = imx386->ctrl_handler.lock;
	imx386->sd.internal_ops = &imx386_internal_ops;
	imx386->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx386->sd.entity.ops = &imx386_subdev_entity_ops;
	imx386->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	imx386->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&imx386->sd.entity, 1, &imx386->pad);
	if (ret) {
		dev_err_probe(imx386->dev, ret,
			      "failed to init media entity pads\n");
		goto v4l2_ctrl_handler_free;
	}

	ret = v4l2_subdev_init_finalize(&imx386->sd);
	if (ret < 0) {
		dev_err_probe(imx386->dev, ret, "failed to init subdev\n");
		goto media_entity_cleanup;
	}

	pm_runtime_set_active(imx386->dev);
	pm_runtime_enable(imx386->dev);

	ret = v4l2_async_register_subdev_sensor(&imx386->sd);
	if (ret < 0) {
		dev_err_probe(imx386->dev, ret,
			      "failed to register V4L2 subdev\n");
		goto subdev_cleanup;
	}

	pm_runtime_set_autosuspend_delay(imx386->dev, 1000);
	pm_runtime_use_autosuspend(imx386->dev);
	pm_runtime_idle(imx386->dev);

	return 0;

subdev_cleanup:
	v4l2_subdev_cleanup(&imx386->sd);
	pm_runtime_disable(imx386->dev);
	pm_runtime_set_suspended(imx386->dev);

media_entity_cleanup:
	media_entity_cleanup(&imx386->sd.entity);

v4l2_ctrl_handler_free:
	v4l2_ctrl_handler_free(imx386->sd.ctrl_handler);

power_off:
	imx386_power_off(imx386->dev);

	return ret;
}

static void imx386_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx386 *imx386 = to_imx386(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
	pm_runtime_disable(imx386->dev);

	if (!pm_runtime_status_suspended(imx386->dev)) {
		imx386_power_off(imx386->dev);
		pm_runtime_set_suspended(imx386->dev);
	}
}

static const struct dev_pm_ops imx386_pm_ops = {
	SET_RUNTIME_PM_OPS(imx386_power_off, imx386_power_on, NULL)
};

static const struct of_device_id imx386_of_match[] = {
	{ .compatible = "sony,imx386" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx386_of_match);

static struct i2c_driver imx386_i2c_driver = {
	.driver = {
		.name = "imx386",
		.pm = &imx386_pm_ops,
		.of_match_table = imx386_of_match,
	},
	.probe = imx386_probe,
	.remove = imx386_remove,
};

module_i2c_driver(imx386_i2c_driver);

MODULE_DESCRIPTION("Sony IMX386 image sensor driver");
MODULE_LICENSE("GPL");
