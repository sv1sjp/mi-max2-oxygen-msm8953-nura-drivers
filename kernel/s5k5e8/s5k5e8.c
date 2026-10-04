// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung S5K5E8 image sensor driver
 *
 * 5 MP (2608x1960 array) RAW10 sensor over 2 MIPI CSI-2 lanes, used as the
 * front camera of the Xiaomi Mi Max 2 (oxygen). The sensor reports SMIA++
 * limits (gain, timing, binning) but programs its PLL in a Samsung-specific
 * way, so the generic CCS driver cannot drive it.
 *
 * Register settings were taken from the Xiaomi Mi Max 2 vendor sensor libraries
 * (libmmcamera_oxygen_s5k5e8_{qtech,ofilm}.so); register meanings follow the
 * SMIA++ limits read from the sensor and Xiaomi's GPL MediaTek S5K5E8 driver.
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

#define S5K5E8_MCLK_FREQ_24MHZ		(24 * HZ_PER_MHZ)
#define S5K5E8_DATA_LANES		2
/* 836 Mbps per lane (0x0820 = 0x0344) */
#define S5K5E8_LINK_FREQ		(418 * HZ_PER_MHZ)

#define S5K5E8_REG_CHIP_ID		CCI_REG16(0x0000)
#define S5K5E8_CHIP_ID			0x5e80

#define S5K5E8_REG_MODE_SELECT		CCI_REG8(0x0100)
#define S5K5E8_MODE_STANDBY		0x00
#define S5K5E8_MODE_STREAMING		0x01

#define S5K5E8_REG_ORIENTATION		CCI_REG8(0x0101)
#define S5K5E8_HFLIP			BIT(0)
#define S5K5E8_VFLIP			BIT(1)

#define S5K5E8_REG_EXPOSURE		CCI_REG16(0x0202)
#define S5K5E8_EXPOSURE_MIN		2
#define S5K5E8_EXPOSURE_STEP		1
#define S5K5E8_EXPOSURE_DEFAULT		990
#define S5K5E8_EXPOSURE_MARGIN		6

/* Analogue gain = code / 32, 1x..16x (SMIA++ m0 = 1, c1 = 32) */
#define S5K5E8_REG_AGAIN		CCI_REG16(0x0204)
#define S5K5E8_AGAIN_MIN		32
#define S5K5E8_AGAIN_MAX		512
#define S5K5E8_AGAIN_STEP		1
#define S5K5E8_AGAIN_DEFAULT		32

/* Per-channel digital gains (Gr, R, B, Gb), 0x100 = 1x */
#define S5K5E8_REG_DGAIN_GR		CCI_REG16(0x020e)
#define S5K5E8_REG_DGAIN_R		CCI_REG16(0x0210)
#define S5K5E8_REG_DGAIN_B		CCI_REG16(0x0212)
#define S5K5E8_REG_DGAIN_GB		CCI_REG16(0x0214)
#define S5K5E8_DGAIN_MIN		0x100
#define S5K5E8_DGAIN_MAX		0x800
#define S5K5E8_DGAIN_STEP		1
#define S5K5E8_DGAIN_DEFAULT		0x100

#define S5K5E8_REG_VTS			CCI_REG16(0x0340)
#define S5K5E8_VTS_MAX			0xffff

#define S5K5E8_REG_TEST_PATTERN		CCI_REG16(0x0600)

#define S5K5E8_NATIVE_WIDTH		2608
#define S5K5E8_NATIVE_HEIGHT		1960

#define to_s5k5e8(_sd)			container_of(_sd, struct s5k5e8, sd)

#include "tables.h"

static const s64 s5k5e8_link_freq_menu[] = {
	S5K5E8_LINK_FREQ,
};

/* Indexed by the ORIENTATION register flip bits; the array starts with Gr */
static const u32 s5k5e8_mbus_formats[] = {
	MEDIA_BUS_FMT_SGRBG10_1X10,
	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SBGGR10_1X10,
	MEDIA_BUS_FMT_SGBRG10_1X10,
};

struct s5k5e8_mode {
	u32 width;
	u32 height;
	u32 hts;
	u32 vts;
	u64 pixel_rate;
	struct v4l2_rect crop;
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

static const struct s5k5e8_mode s5k5e8_modes[] = {
	{
		/* Full resolution, ~29.8 fps */
		.width = 2592,
		.height = 1944,
		.hts = 3136,
		.vts = 1968,
		.pixel_rate = 184000000,
		.crop = { 8, 8, 2592, 1944 },
		.regs = s5k5e8_mode_2592x1944_regs,
		.num_regs = ARRAY_SIZE(s5k5e8_mode_2592x1944_regs),
	},
};

static const char * const s5k5e8_test_pattern_menu[] = {
	"Disabled",
	"Solid colour",
	"Colour bars",
	"Fade to grey colour bars",
	"PN9",
};

/* In power-up order (vendor: VDIG, VANA, VIO) */
static const char * const s5k5e8_supply_names[] = {
	"dvdd",		/* Digital core power, 1.2 V */
	"avdd",		/* Analog power, 2.8 V */
	"dovdd",	/* Digital I/O power, 1.8 V */
};

#define S5K5E8_NUM_SUPPLIES	ARRAY_SIZE(s5k5e8_supply_names)

struct s5k5e8 {
	struct device *dev;
	struct regmap *regmap;
	struct clk *mclk;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *standby_gpio;
	struct regulator_bulk_data supplies[S5K5E8_NUM_SUPPLIES];

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

	const struct s5k5e8_mode *mode;
};

static u32 s5k5e8_get_format_code(struct s5k5e8 *s5k5e8)
{
	unsigned int i;

	i = (s5k5e8->vflip->val ? S5K5E8_VFLIP : 0) |
	    (s5k5e8->hflip->val ? S5K5E8_HFLIP : 0);

	return s5k5e8_mbus_formats[i];
}

static int s5k5e8_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct s5k5e8 *s5k5e8 = container_of(ctrl->handler, struct s5k5e8,
					     ctrl_handler);
	const struct s5k5e8_mode *mode = s5k5e8->mode;
	struct v4l2_subdev_state *state;
	s64 exposure_max;
	int ret = 0;

	switch (ctrl->id) {
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		/* Flipping changes the Bayer order of the active format */
		if (s5k5e8->sd.active_state) {
			state = v4l2_subdev_get_locked_active_state(&s5k5e8->sd);
			v4l2_subdev_state_get_format(state, 0)->code =
				s5k5e8_get_format_code(s5k5e8);
		}
		break;
	case V4L2_CID_VBLANK:
		exposure_max = mode->height + ctrl->val - S5K5E8_EXPOSURE_MARGIN;
		__v4l2_ctrl_modify_range(s5k5e8->exposure,
					 s5k5e8->exposure->minimum,
					 exposure_max,
					 s5k5e8->exposure->step,
					 min(s5k5e8->exposure->default_value,
					     exposure_max));
		break;
	}

	/* V4L2 controls are applied when the sensor is powered up */
	if (!pm_runtime_get_if_active(s5k5e8->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(s5k5e8->regmap, S5K5E8_REG_AGAIN, ctrl->val, &ret);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		cci_write(s5k5e8->regmap, S5K5E8_REG_DGAIN_GR, ctrl->val, &ret);
		cci_write(s5k5e8->regmap, S5K5E8_REG_DGAIN_R, ctrl->val, &ret);
		cci_write(s5k5e8->regmap, S5K5E8_REG_DGAIN_B, ctrl->val, &ret);
		cci_write(s5k5e8->regmap, S5K5E8_REG_DGAIN_GB, ctrl->val, &ret);
		break;
	case V4L2_CID_EXPOSURE:
		cci_write(s5k5e8->regmap, S5K5E8_REG_EXPOSURE, ctrl->val, &ret);
		break;
	case V4L2_CID_VBLANK:
		cci_write(s5k5e8->regmap, S5K5E8_REG_VTS,
			  mode->height + ctrl->val, &ret);
		break;
	case V4L2_CID_TEST_PATTERN:
		cci_write(s5k5e8->regmap, S5K5E8_REG_TEST_PATTERN,
			  ctrl->val, &ret);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		cci_write(s5k5e8->regmap, S5K5E8_REG_ORIENTATION,
			  (s5k5e8->hflip->val ? S5K5E8_HFLIP : 0) |
			  (s5k5e8->vflip->val ? S5K5E8_VFLIP : 0), &ret);
		break;
	case V4L2_CID_LINK_FREQ:
	case V4L2_CID_PIXEL_RATE:
	case V4L2_CID_HBLANK:
		/* Fixed by the mode, written with the mode registers */
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(s5k5e8->dev);

	return ret;
}

static const struct v4l2_ctrl_ops s5k5e8_ctrl_ops = {
	.s_ctrl = s5k5e8_set_ctrl,
};

static int s5k5e8_init_controls(struct s5k5e8 *s5k5e8)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &s5k5e8->ctrl_handler;
	const struct s5k5e8_mode *mode = s5k5e8->mode;
	struct v4l2_fwnode_device_properties props;
	s64 hblank, vblank, exposure_max;
	int ret;

	v4l2_ctrl_handler_init(ctrl_hdlr, 12);

	s5k5e8->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, &s5k5e8_ctrl_ops,
					V4L2_CID_LINK_FREQ,
					ARRAY_SIZE(s5k5e8_link_freq_menu) - 1,
					0, s5k5e8_link_freq_menu);
	if (s5k5e8->link_freq)
		s5k5e8->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	s5k5e8->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e8_ctrl_ops,
					       V4L2_CID_PIXEL_RATE,
					       mode->pixel_rate,
					       mode->pixel_rate, 1,
					       mode->pixel_rate);

	hblank = mode->hts - mode->width;
	s5k5e8->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e8_ctrl_ops,
					   V4L2_CID_HBLANK, hblank,
					   hblank, 1, hblank);
	if (s5k5e8->hblank)
		s5k5e8->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank = mode->vts - mode->height;
	s5k5e8->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e8_ctrl_ops,
					   V4L2_CID_VBLANK, vblank,
					   S5K5E8_VTS_MAX - mode->height, 1,
					   vblank);

	v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e8_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  S5K5E8_AGAIN_MIN, S5K5E8_AGAIN_MAX,
			  S5K5E8_AGAIN_STEP, S5K5E8_AGAIN_DEFAULT);

	v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e8_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
			  S5K5E8_DGAIN_MIN, S5K5E8_DGAIN_MAX,
			  S5K5E8_DGAIN_STEP, S5K5E8_DGAIN_DEFAULT);

	exposure_max = mode->vts - S5K5E8_EXPOSURE_MARGIN;
	s5k5e8->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e8_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     S5K5E8_EXPOSURE_MIN,
					     exposure_max,
					     S5K5E8_EXPOSURE_STEP,
					     S5K5E8_EXPOSURE_DEFAULT);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &s5k5e8_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(s5k5e8_test_pattern_menu) - 1,
				     0, 0, s5k5e8_test_pattern_menu);

	s5k5e8->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e8_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (s5k5e8->hflip)
		s5k5e8->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	s5k5e8->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e8_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (s5k5e8->vflip)
		s5k5e8->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		goto error_free_hdlr;
	}

	ret = v4l2_fwnode_device_parse(s5k5e8->dev, &props);
	if (ret)
		goto error_free_hdlr;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &s5k5e8_ctrl_ops,
					      &props);
	if (ret)
		goto error_free_hdlr;

	s5k5e8->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error_free_hdlr:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static int s5k5e8_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct s5k5e8 *s5k5e8 = to_s5k5e8(sd);
	const struct s5k5e8_mode *mode = s5k5e8->mode;
	int ret;

	ret = pm_runtime_resume_and_get(s5k5e8->dev);
	if (ret)
		return ret;

	cci_multi_reg_write(s5k5e8->regmap, s5k5e8_init_regs,
			    ARRAY_SIZE(s5k5e8_init_regs), &ret);
	cci_multi_reg_write(s5k5e8->regmap, mode->regs, mode->num_regs, &ret);
	if (ret)
		goto error;

	/* The mode table sets exposure and orientation, controls restore them */
	ret = __v4l2_ctrl_handler_setup(s5k5e8->sd.ctrl_handler);
	if (ret)
		goto error;

	ret = cci_write(s5k5e8->regmap, S5K5E8_REG_MODE_SELECT,
			S5K5E8_MODE_STREAMING, NULL);
	if (ret)
		goto error;

	/* Flipping changes the Bayer order, don't allow it while streaming */
	__v4l2_ctrl_grab(s5k5e8->vflip, true);
	__v4l2_ctrl_grab(s5k5e8->hflip, true);

	return 0;

error:
	dev_err(s5k5e8->dev, "failed to start streaming: %d\n", ret);
	pm_runtime_put_autosuspend(s5k5e8->dev);

	return ret;
}

static int s5k5e8_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct s5k5e8 *s5k5e8 = to_s5k5e8(sd);
	int ret;

	ret = cci_write(s5k5e8->regmap, S5K5E8_REG_MODE_SELECT,
			S5K5E8_MODE_STANDBY, NULL);
	if (ret)
		dev_err(s5k5e8->dev, "failed to stop streaming: %d\n", ret);

	__v4l2_ctrl_grab(s5k5e8->vflip, false);
	__v4l2_ctrl_grab(s5k5e8->hflip, false);

	pm_runtime_put_autosuspend(s5k5e8->dev);

	return ret;
}

static void s5k5e8_update_pad_format(struct s5k5e8 *s5k5e8,
				     const struct s5k5e8_mode *mode,
				     struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = s5k5e8_get_format_code(s5k5e8);
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int s5k5e8_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct s5k5e8 *s5k5e8 = to_s5k5e8(sd);
	const struct s5k5e8_mode *mode;

	mode = v4l2_find_nearest_size(s5k5e8_modes, ARRAY_SIZE(s5k5e8_modes),
				      width, height, fmt->format.width,
				      fmt->format.height);

	s5k5e8_update_pad_format(s5k5e8, mode, &fmt->format);
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;
	*v4l2_subdev_state_get_crop(state, 0) = mode->crop;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		s5k5e8->mode = mode;

	return 0;
}

static int s5k5e8_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct s5k5e8 *s5k5e8 = to_s5k5e8(sd);

	if (code->index > 0)
		return -EINVAL;

	code->code = s5k5e8_get_format_code(s5k5e8);

	return 0;
}

static int s5k5e8_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct s5k5e8 *s5k5e8 = to_s5k5e8(sd);

	if (fse->code != s5k5e8_get_format_code(s5k5e8) ||
	    fse->index >= ARRAY_SIZE(s5k5e8_modes))
		return -EINVAL;

	fse->min_width = s5k5e8_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = s5k5e8_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static int s5k5e8_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(sd_state, 0);
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = S5K5E8_NATIVE_WIDTH;
		sel->r.height = S5K5E8_NATIVE_HEIGHT;
		return 0;
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r = s5k5e8_modes[0].crop;
		return 0;
	default:
		return -EINVAL;
	}
}

static int s5k5e8_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.width = s5k5e8_modes[0].width,
			.height = s5k5e8_modes[0].height,
		},
	};

	return s5k5e8_set_pad_format(sd, state, &fmt);
}

static const struct v4l2_subdev_video_ops s5k5e8_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops s5k5e8_pad_ops = {
	.set_fmt = s5k5e8_set_pad_format,
	.get_fmt = v4l2_subdev_get_fmt,
	.get_selection = s5k5e8_get_selection,
	.enum_mbus_code = s5k5e8_enum_mbus_code,
	.enum_frame_size = s5k5e8_enum_frame_size,
	.enable_streams = s5k5e8_enable_streams,
	.disable_streams = s5k5e8_disable_streams,
};

static const struct v4l2_subdev_ops s5k5e8_subdev_ops = {
	.video = &s5k5e8_video_ops,
	.pad = &s5k5e8_pad_ops,
};

static const struct v4l2_subdev_internal_ops s5k5e8_internal_ops = {
	.init_state = s5k5e8_init_state,
};

static const struct media_entity_operations s5k5e8_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int s5k5e8_identify_sensor(struct s5k5e8 *s5k5e8)
{
	u64 val;
	int ret;

	ret = cci_read(s5k5e8->regmap, S5K5E8_REG_CHIP_ID, &val, NULL);
	if (ret)
		return dev_err_probe(s5k5e8->dev, ret,
				     "failed to read chip id\n");

	if (val != S5K5E8_CHIP_ID)
		return dev_err_probe(s5k5e8->dev, -ENODEV,
				     "chip id mismatch: %x!=%llx\n",
				     S5K5E8_CHIP_ID, val);

	return 0;
}

static int s5k5e8_check_hwcfg(struct s5k5e8 *s5k5e8)
{
	struct fwnode_handle *fwnode = dev_fwnode(s5k5e8->dev), *ep;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	unsigned long link_freq_bitmap;
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

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != S5K5E8_DATA_LANES) {
		ret = dev_err_probe(s5k5e8->dev, -EINVAL,
				    "invalid number of data lanes: %u\n",
				    bus_cfg.bus.mipi_csi2.num_data_lanes);
		goto endpoint_free;
	}

	ret = v4l2_link_freq_to_bitmap(s5k5e8->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       s5k5e8_link_freq_menu,
				       ARRAY_SIZE(s5k5e8_link_freq_menu),
				       &link_freq_bitmap);

endpoint_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int s5k5e8_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5k5e8 *s5k5e8 = to_s5k5e8(sd);
	unsigned int i;
	int ret;

	gpiod_set_value_cansleep(s5k5e8->reset_gpio, 1);
	gpiod_set_value_cansleep(s5k5e8->standby_gpio, 1);

	/* Vendor sequence: VDIG, VANA, VIO */
	for (i = 0; i < S5K5E8_NUM_SUPPLIES; i++) {
		ret = regulator_enable(s5k5e8->supplies[i].consumer);
		if (ret)
			goto disable_regulators;
		usleep_range(1000, 1100);
	}

	ret = clk_prepare_enable(s5k5e8->mclk);
	if (ret)
		goto disable_regulators;

	usleep_range(1000, 1100);
	gpiod_set_value_cansleep(s5k5e8->reset_gpio, 0);
	usleep_range(10 * USEC_PER_MSEC, 11 * USEC_PER_MSEC);
	gpiod_set_value_cansleep(s5k5e8->standby_gpio, 0);
	usleep_range(10 * USEC_PER_MSEC, 11 * USEC_PER_MSEC);

	return 0;

disable_regulators:
	while (i--)
		regulator_disable(s5k5e8->supplies[i].consumer);

	return ret;
}

static int s5k5e8_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5k5e8 *s5k5e8 = to_s5k5e8(sd);
	unsigned int i;

	gpiod_set_value_cansleep(s5k5e8->standby_gpio, 1);
	gpiod_set_value_cansleep(s5k5e8->reset_gpio, 1);

	clk_disable_unprepare(s5k5e8->mclk);

	for (i = S5K5E8_NUM_SUPPLIES; i--;)
		regulator_disable(s5k5e8->supplies[i].consumer);

	return 0;
}

static int s5k5e8_probe(struct i2c_client *client)
{
	struct s5k5e8 *s5k5e8;
	unsigned long freq;
	unsigned int i;
	int ret;

	s5k5e8 = devm_kzalloc(&client->dev, sizeof(*s5k5e8), GFP_KERNEL);
	if (!s5k5e8)
		return -ENOMEM;

	s5k5e8->dev = &client->dev;
	v4l2_i2c_subdev_init(&s5k5e8->sd, client, &s5k5e8_subdev_ops);

	s5k5e8->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(s5k5e8->regmap))
		return dev_err_probe(s5k5e8->dev, PTR_ERR(s5k5e8->regmap),
				     "failed to init CCI\n");

	s5k5e8->mclk = devm_v4l2_sensor_clk_get(s5k5e8->dev, NULL);
	if (IS_ERR(s5k5e8->mclk))
		return dev_err_probe(s5k5e8->dev, PTR_ERR(s5k5e8->mclk),
				     "failed to get MCLK clock\n");

	freq = clk_get_rate(s5k5e8->mclk);
	if (freq != S5K5E8_MCLK_FREQ_24MHZ)
		return dev_err_probe(s5k5e8->dev, -EINVAL,
				     "MCLK clock frequency %lu is not supported\n",
				     freq);

	ret = s5k5e8_check_hwcfg(s5k5e8);
	if (ret)
		return dev_err_probe(s5k5e8->dev, ret,
				     "failed to check HW configuration\n");

	s5k5e8->reset_gpio = devm_gpiod_get_optional(s5k5e8->dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(s5k5e8->reset_gpio))
		return dev_err_probe(s5k5e8->dev, PTR_ERR(s5k5e8->reset_gpio),
				     "cannot get reset GPIO\n");

	s5k5e8->standby_gpio = devm_gpiod_get_optional(s5k5e8->dev, "standby",
						       GPIOD_OUT_HIGH);
	if (IS_ERR(s5k5e8->standby_gpio))
		return dev_err_probe(s5k5e8->dev, PTR_ERR(s5k5e8->standby_gpio),
				     "cannot get standby GPIO\n");

	for (i = 0; i < S5K5E8_NUM_SUPPLIES; i++)
		s5k5e8->supplies[i].supply = s5k5e8_supply_names[i];

	ret = devm_regulator_bulk_get(s5k5e8->dev, S5K5E8_NUM_SUPPLIES,
				      s5k5e8->supplies);
	if (ret)
		return dev_err_probe(s5k5e8->dev, ret,
				     "failed to get supply regulators\n");

	/* The sensor must be powered on to read the CHIP_ID register */
	ret = s5k5e8_power_on(s5k5e8->dev);
	if (ret)
		return ret;

	ret = s5k5e8_identify_sensor(s5k5e8);
	if (ret)
		goto power_off;

	s5k5e8->mode = &s5k5e8_modes[0];
	ret = s5k5e8_init_controls(s5k5e8);
	if (ret) {
		dev_err_probe(s5k5e8->dev, ret, "failed to init controls\n");
		goto power_off;
	}

	s5k5e8->sd.state_lock = s5k5e8->ctrl_handler.lock;
	s5k5e8->sd.internal_ops = &s5k5e8_internal_ops;
	s5k5e8->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	s5k5e8->sd.entity.ops = &s5k5e8_subdev_entity_ops;
	s5k5e8->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	s5k5e8->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&s5k5e8->sd.entity, 1, &s5k5e8->pad);
	if (ret) {
		dev_err_probe(s5k5e8->dev, ret,
			      "failed to init media entity pads\n");
		goto v4l2_ctrl_handler_free;
	}

	ret = v4l2_subdev_init_finalize(&s5k5e8->sd);
	if (ret < 0) {
		dev_err_probe(s5k5e8->dev, ret, "failed to init subdev\n");
		goto media_entity_cleanup;
	}

	pm_runtime_set_active(s5k5e8->dev);
	pm_runtime_enable(s5k5e8->dev);

	ret = v4l2_async_register_subdev_sensor(&s5k5e8->sd);
	if (ret < 0) {
		dev_err_probe(s5k5e8->dev, ret,
			      "failed to register V4L2 subdev\n");
		goto subdev_cleanup;
	}

	pm_runtime_set_autosuspend_delay(s5k5e8->dev, 1000);
	pm_runtime_use_autosuspend(s5k5e8->dev);
	pm_runtime_idle(s5k5e8->dev);

	return 0;

subdev_cleanup:
	v4l2_subdev_cleanup(&s5k5e8->sd);
	pm_runtime_disable(s5k5e8->dev);
	pm_runtime_set_suspended(s5k5e8->dev);

media_entity_cleanup:
	media_entity_cleanup(&s5k5e8->sd.entity);

v4l2_ctrl_handler_free:
	v4l2_ctrl_handler_free(s5k5e8->sd.ctrl_handler);

power_off:
	s5k5e8_power_off(s5k5e8->dev);

	return ret;
}

static void s5k5e8_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct s5k5e8 *s5k5e8 = to_s5k5e8(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
	pm_runtime_disable(s5k5e8->dev);

	if (!pm_runtime_status_suspended(s5k5e8->dev)) {
		s5k5e8_power_off(s5k5e8->dev);
		pm_runtime_set_suspended(s5k5e8->dev);
	}
}

static const struct dev_pm_ops s5k5e8_pm_ops = {
	SET_RUNTIME_PM_OPS(s5k5e8_power_off, s5k5e8_power_on, NULL)
};

static const struct of_device_id s5k5e8_of_match[] = {
	{ .compatible = "samsung,s5k5e8" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, s5k5e8_of_match);

static struct i2c_driver s5k5e8_i2c_driver = {
	.driver = {
		.name = "s5k5e8",
		.pm = &s5k5e8_pm_ops,
		.of_match_table = s5k5e8_of_match,
	},
	.probe = s5k5e8_probe,
	.remove = s5k5e8_remove,
};

module_i2c_driver(s5k5e8_i2c_driver);

MODULE_DESCRIPTION("Samsung S5K5E8 image sensor driver");
MODULE_LICENSE("GPL");
