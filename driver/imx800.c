// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony IMX800 camera sensor driver (Fairphone 5 bring-up)
 *
 * Single mode (from vendor sensor module RE):
 *   4096x3072, 10-bit BGGR, 2x2 bin from QCFA 8192x6144
 *   FLL=7484, LLP=9776, CSI-2 C-PHY 3-trio @ 700 MHz link frequency
 *
 * Requires DT power rails + reset, and a CSIPHY path that supports C-PHY
 * (on SM7325/sc7280: camss C-PHY sequence for qcom,csiphy-v1.2.1).
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#include "imx800_mode_tables.h"

#define IMX800_REG_CHIP_ID		CCI_REG16(0x0016)
#define IMX800_CHIP_ID_EXPECT		0x0800
#define IMX800_REG_MODE_SELECT		CCI_REG8(0x0100)
#define IMX800_MODE_STANDBY		0x00
#define IMX800_MODE_STREAMING		0x01
#define IMX800_INCLK_RATE		24000000
#define IMX800_MODE1_VBLANK		(IMX800_MODE1_FLL - IMX800_MODE1_HEIGHT)
/* Mode-1 link frequency (Hz); keep ≤ timer limits in camss. */
#define IMX800_LINK_FREQ_700MHZ		700000000ULL
/* Approximate pixel rate for V4L2_CID_PIXEL_RATE. */
#define IMX800_PIXEL_RATE		450000000ULL

static int xclr_gpio = -1;
module_param_named(xclr, xclr_gpio, int, 0644);
MODULE_PARM_DESC(xclr, "TLMM pin offset for XCLR (active low), -1=DT/none");

static int try_stream = 1;
module_param(try_stream, int, 0644);
MODULE_PARM_DESC(try_stream,
		 "If 1 (default), s_stream programs init+mode1 tables and MODE_SELECT");

static int program_on_probe = 0;
module_param(program_on_probe, int, 0644);
MODULE_PARM_DESC(program_on_probe,
		 "If 1, write init+mode1+stream-on during probe (CCI validation)");

/* Degrees clockwise for V4L2_CID_CAMERA_SENSOR_ROTATION (default matches board DT). */
static int rotation = 270;
module_param(rotation, int, 0644);
MODULE_PARM_DESC(rotation,
		 "Sensor rotation degrees clockwise (0/90/180/270); default 270");

static const char * const imx800_supply_names[] = {
	"vana1", "vana2", "vdig", "vif",
};

static const s64 imx800_link_freq_menu[] = {
	IMX800_LINK_FREQ_700MHZ,
};

struct imx800 {
	struct device *dev;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct gpio_desc *reset_gpio;
	bool reset_own;
	struct clk *inclk;
	struct regulator_bulk_data supplies[ARRAY_SIZE(imx800_supply_names)];
	struct regmap *regmap;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
};

static inline struct imx800 *to_imx800(struct v4l2_subdev *sd)
{
	return container_of(sd, struct imx800, sd);
}

static int imx800_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx800 *imx800 = to_imx800(sd);
	int ret;

	/* Hold reset asserted (ACTIVE_LOW desc: 1 = assert) */
	if (imx800->reset_gpio)
		gpiod_set_value_cansleep(imx800->reset_gpio, 1);

	ret = regulator_bulk_enable(ARRAY_SIZE(imx800_supply_names),
				    imx800->supplies);
	if (ret) {
		dev_err(dev, "regulators enable failed: %d\n", ret);
		return ret;
	}
	usleep_range(1000, 2000);

	ret = clk_prepare_enable(imx800->inclk);
	if (ret) {
		dev_err(dev, "inclk enable failed: %d\n", ret);
		regulator_bulk_disable(ARRAY_SIZE(imx800_supply_names),
				       imx800->supplies);
		return ret;
	}
	usleep_range(1000, 2000);

	/* Release XCLR */
	if (imx800->reset_gpio)
		gpiod_set_value_cansleep(imx800->reset_gpio, 0);
	msleep(15);

	dev_info(dev, "power on complete (xclr %s)\n",
		 imx800->reset_gpio ? "driven" : "absent");
	return 0;
}

static int imx800_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx800 *imx800 = to_imx800(sd);

	if (imx800->reset_gpio)
		gpiod_set_value_cansleep(imx800->reset_gpio, 1);
	clk_disable_unprepare(imx800->inclk);
	regulator_bulk_disable(ARRAY_SIZE(imx800_supply_names),
			       imx800->supplies);
	return 0;
}

static int imx800_detect(struct imx800 *imx800)
{
	u64 val = 0;
	int ret, attempt;

	for (attempt = 0; attempt < 5; attempt++) {
		ret = cci_read(imx800->regmap, IMX800_REG_CHIP_ID, &val, NULL);
		if (!ret)
			break;
		dev_warn(imx800->dev, "chip id try %d: %d\n", attempt + 1, ret);
		msleep(5);
	}
	if (ret) {
		dev_err(imx800->dev, "chip id failed: %d\n", ret);
		return ret;
	}
	dev_info(imx800->dev, "IMX800 chip id 0x%04llx (expect 0x%04x)\n",
		 val, IMX800_CHIP_ID_EXPECT);
	dev_info(imx800->dev,
		 "mode1 tables: %ux%u FLL=%u LLP=%u init=%zu mode=%zu (mode tables)\n",
		 IMX800_MODE1_WIDTH, IMX800_MODE1_HEIGHT,
		 IMX800_MODE1_FLL, IMX800_MODE1_LLP,
		 ARRAY_SIZE(imx800_init_regs), ARRAY_SIZE(imx800_mode1_regs));
	return 0;
}

static void imx800_fill_format(struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = IMX800_MODE1_WIDTH;
	fmt->height = IMX800_MODE1_HEIGHT;
	/* Android camera0 colorFilterArrangement = BGGR (not RGGB). */
	fmt->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int imx800_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	return 0;
}

static int imx800_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fsize)
{
	if (fsize->index > 0)
		return -EINVAL;
	if (fsize->code != MEDIA_BUS_FMT_SBGGR10_1X10)
		return -EINVAL;

	fsize->min_width = IMX800_MODE1_WIDTH;
	fsize->max_width = IMX800_MODE1_WIDTH;
	fsize->min_height = IMX800_MODE1_HEIGHT;
	fsize->max_height = IMX800_MODE1_HEIGHT;
	return 0;
}

static int imx800_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *try_fmt;

		try_fmt = v4l2_subdev_state_get_format(state, fmt->pad);
		fmt->format = *try_fmt;
	} else {
		imx800_fill_format(&fmt->format);
	}
	return 0;
}

static int imx800_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	/* Single fixed mode: always report mode-1. */
	imx800_fill_format(&fmt->format);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *try_fmt;

		try_fmt = v4l2_subdev_state_get_format(state, fmt->pad);
		*try_fmt = fmt->format;
	}
	return 0;
}

static int imx800_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	/* Native array is QCFA 8192x6144; mode-1 is 2x2 binned 4096x3072. */
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = IMX800_MODE1_WIDTH;
		sel->r.height = IMX800_MODE1_HEIGHT;
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = 8192;
		sel->r.height = 6144;
		return 0;
	default:
		return -EINVAL;
	}
}

static int imx800_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *fmt;

	fmt = v4l2_subdev_state_get_format(state, 0);
	imx800_fill_format(fmt);
	return 0;
}

static int imx800_write_mode1(struct imx800 *imx800)
{
	int ret = 0;

	ret = cci_multi_reg_write(imx800->regmap, imx800_init_regs,
				  ARRAY_SIZE(imx800_init_regs), NULL);
	if (ret) {
		dev_err(imx800->dev, "init regs failed: %d\n", ret);
		return ret;
	}
	usleep_range(50, 100);

	ret = cci_multi_reg_write(imx800->regmap, imx800_mode1_regs,
				  ARRAY_SIZE(imx800_mode1_regs), NULL);
	if (ret) {
		dev_err(imx800->dev, "mode1 regs failed: %d\n", ret);
		return ret;
	}
	return 0;
}

static int imx800_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx800 *imx800 = to_imx800(sd);
	int ret = 0;

	if (!try_stream) {
		dev_info(imx800->dev,
			 "s_stream(%d): try_stream=0 (set try_stream=1 to program mode tables)\n",
			 enable);
		return enable ? -EOPNOTSUPP : 0;
	}

	if (enable) {
		ret = pm_runtime_resume_and_get(imx800->dev);
		if (ret < 0) {
			dev_err(imx800->dev, "runtime resume: %d\n", ret);
			return ret;
		}
		/* Re-power may leave sensor in reset; re-detect then program */
		ret = imx800_detect(imx800);
		if (!ret)
			ret = imx800_write_mode1(imx800);
		if (!ret) {
			/* allow PLL settle before MODE_SELECT streaming */
			msleep(10);
			ret = cci_multi_reg_write(imx800->regmap,
						  imx800_stream_on_regs,
						  ARRAY_SIZE(imx800_stream_on_regs),
						  NULL);
		}
		if (!ret) {
			u64 mode = 0;
			cci_read(imx800->regmap, IMX800_REG_MODE_SELECT, &mode, NULL);
			dev_info(imx800->dev, "MODE_SELECT after stream-on = 0x%llx\n", mode);
		}
		dev_info(imx800->dev,
			 "s_stream on: init+mode1+0x0100 ret=%d\n", ret);
		if (ret)
			pm_runtime_put(imx800->dev);
	} else {
		ret = cci_multi_reg_write(imx800->regmap, imx800_stream_off_regs,
					  ARRAY_SIZE(imx800_stream_off_regs),
					  NULL);
		dev_info(imx800->dev, "s_stream off: MODE ret=%d\n", ret);
		pm_runtime_put_autosuspend(imx800->dev);
	}
	return ret;
}

static const struct v4l2_subdev_video_ops imx800_video_ops = {
	.s_stream = imx800_s_stream,
};
static const struct v4l2_subdev_pad_ops imx800_pad_ops = {
	.enum_mbus_code = imx800_enum_mbus_code,
	.enum_frame_size = imx800_enum_frame_size,
	.get_fmt = imx800_get_fmt,
	.set_fmt = imx800_set_fmt,
	.get_selection = imx800_get_selection,
};
static const struct v4l2_subdev_ops imx800_subdev_ops = {
	.video = &imx800_video_ops,
	.pad = &imx800_pad_ops,
};
static const struct v4l2_subdev_internal_ops imx800_internal_ops = {
	.init_state = imx800_init_state,
};

static int imx800_probe(struct i2c_client *client)
{
	struct imx800 *imx800;
	unsigned long rate;
	unsigned int i;
	int ret;

	imx800 = devm_kzalloc(&client->dev, sizeof(*imx800), GFP_KERNEL);
	if (!imx800)
		return -ENOMEM;

	imx800->dev = &client->dev;
	dev_info(imx800->dev, "probe enter xclr=%d\n", xclr_gpio);
	v4l2_i2c_subdev_init(&imx800->sd, client, &imx800_subdev_ops);

	if (xclr_gpio >= 0) {
		struct gpio_device *gdev;
		struct gpio_chip *chip;

		gdev = gpio_device_find_by_label("f100000.pinctrl");
		if (!gdev) {
			dev_err(imx800->dev, "gpio_device f100000.pinctrl not found\n");
			return -ENODEV;
		}
		chip = gpio_device_get_chip(gdev);
		dev_info(imx800->dev, "TLMM chip base=%d ngpio=%u\n",
			 chip->base, chip->ngpio);
		imx800->reset_gpio = gpiochip_request_own_desc(
			chip, xclr_gpio, "imx800-xclr",
			GPIO_ACTIVE_LOW, GPIOD_OUT_HIGH);
		if (IS_ERR(imx800->reset_gpio)) {
			ret = PTR_ERR(imx800->reset_gpio);
			dev_err(imx800->dev, "TLMM offset %d -> %d\n",
				xclr_gpio, ret);
			imx800->reset_gpio = NULL;
			return ret;
		}
		imx800->reset_own = true;
		dev_info(imx800->dev, "using TLMM xclr offset %d\n", xclr_gpio);
	} else {
		imx800->reset_gpio = devm_gpiod_get_optional(
			imx800->dev, "reset", GPIOD_OUT_HIGH);
		if (IS_ERR(imx800->reset_gpio))
			return PTR_ERR(imx800->reset_gpio);
	}

	imx800->inclk = devm_clk_get(imx800->dev, NULL);
	if (IS_ERR(imx800->inclk))
		return dev_err_probe(imx800->dev, PTR_ERR(imx800->inclk),
				     "inclk\n");

	rate = clk_get_rate(imx800->inclk);
	if (rate && rate != IMX800_INCLK_RATE)
		dev_warn(imx800->dev, "inclk %lu Hz (expected %u)\n",
			 rate, IMX800_INCLK_RATE);

	for (i = 0; i < ARRAY_SIZE(imx800_supply_names); i++)
		imx800->supplies[i].supply = imx800_supply_names[i];

	ret = devm_regulator_bulk_get(imx800->dev,
				      ARRAY_SIZE(imx800_supply_names),
				      imx800->supplies);
	if (ret)
		return dev_err_probe(imx800->dev, ret, "regulators\n");

	imx800->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(imx800->regmap))
		return PTR_ERR(imx800->regmap);

	ret = imx800_power_on(imx800->dev);
	if (ret)
		goto err_free_gpio;

	ret = imx800_detect(imx800);
	if (ret)
		goto err_power;

	if (program_on_probe) {
		ret = imx800_write_mode1(imx800);
		if (!ret)
			ret = cci_multi_reg_write(imx800->regmap,
						  imx800_stream_on_regs,
						  ARRAY_SIZE(imx800_stream_on_regs),
						  NULL);
		dev_info(imx800->dev,
			 "program_on_probe: init+mode1+stream %s (ret=%d)\n",
			 ret ? "FAILED" : "OK", ret);
		/* leave streaming off after validation unless try_stream */
		if (!ret && !try_stream)
			cci_multi_reg_write(imx800->regmap,
					    imx800_stream_off_regs,
					    ARRAY_SIZE(imx800_stream_off_regs),
					    NULL);
		ret = 0; /* do not fail probe on optional program */
	}

	imx800->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx800->sd.internal_ops = &imx800_internal_ops;
	imx800->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	imx800->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&imx800->sd.entity, 1, &imx800->pad);
	if (ret)
		goto err_power;

	/* LINK_FREQ for camss; blanking/orientation for userspace soft-ISP. */
	ret = v4l2_ctrl_handler_init(&imx800->ctrl_handler, 8);
	if (ret)
		goto err_entity;
	imx800->link_freq = v4l2_ctrl_new_int_menu(
		&imx800->ctrl_handler, NULL, V4L2_CID_LINK_FREQ,
		ARRAY_SIZE(imx800_link_freq_menu) - 1, 0,
		imx800_link_freq_menu);
	if (imx800->link_freq)
		imx800->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	imx800->pixel_rate = v4l2_ctrl_new_std(
		&imx800->ctrl_handler, NULL, V4L2_CID_PIXEL_RATE,
		IMX800_PIXEL_RATE, IMX800_PIXEL_RATE, 1, IMX800_PIXEL_RATE);
	/* vblank = FLL - height; hblank derived from LLP - width */
	v4l2_ctrl_new_std(&imx800->ctrl_handler, NULL, V4L2_CID_VBLANK,
			  IMX800_MODE1_VBLANK, IMX800_MODE1_VBLANK, 1,
			  IMX800_MODE1_VBLANK);
	v4l2_ctrl_new_std(&imx800->ctrl_handler, NULL, V4L2_CID_HBLANK,
			  IMX800_MODE1_LLP - IMX800_MODE1_WIDTH,
			  IMX800_MODE1_LLP - IMX800_MODE1_WIDTH, 1,
			  IMX800_MODE1_LLP - IMX800_MODE1_WIDTH);
	/* Fixed exposure/gain placeholders for soft-ISP pipelines. */
	v4l2_ctrl_new_std(&imx800->ctrl_handler, NULL, V4L2_CID_EXPOSURE,
			  1, IMX800_MODE1_FLL - 1, 1, IMX800_MODE1_HEIGHT);
	v4l2_ctrl_new_std(&imx800->ctrl_handler, NULL, V4L2_CID_ANALOGUE_GAIN,
			  0, 0xffff, 1, 0x100);
	v4l2_ctrl_new_std_menu(&imx800->ctrl_handler, NULL,
			       V4L2_CID_CAMERA_ORIENTATION,
			       V4L2_CAMERA_ORIENTATION_BACK, 0,
			       V4L2_CAMERA_ORIENTATION_BACK);
	{
		u32 rot = 270;

		if (rotation == 0 || rotation == 90 || rotation == 180 ||
		    rotation == 270)
			rot = rotation;
		else
			dev_warn(imx800->dev,
				 "invalid rotation=%d, using 270\n", rotation);
		v4l2_ctrl_new_std(&imx800->ctrl_handler, NULL,
				  V4L2_CID_CAMERA_SENSOR_ROTATION,
				  rot, rot, 1, rot);
	}
	if (imx800->ctrl_handler.error) {
		ret = imx800->ctrl_handler.error;
		goto err_ctrls;
	}
	/* Make blanking read-only (fixed mode-1 tables). */
	{
		struct v4l2_ctrl *c;

		c = v4l2_ctrl_find(&imx800->ctrl_handler, V4L2_CID_VBLANK);
		if (c)
			c->flags |= V4L2_CTRL_FLAG_READ_ONLY;
		c = v4l2_ctrl_find(&imx800->ctrl_handler, V4L2_CID_HBLANK);
		if (c)
			c->flags |= V4L2_CTRL_FLAG_READ_ONLY;
		c = v4l2_ctrl_find(&imx800->ctrl_handler,
				   V4L2_CID_CAMERA_ORIENTATION);
		if (c)
			c->flags |= V4L2_CTRL_FLAG_READ_ONLY;
		c = v4l2_ctrl_find(&imx800->ctrl_handler,
				   V4L2_CID_CAMERA_SENSOR_ROTATION);
		if (c)
			c->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	}
	imx800->sd.ctrl_handler = &imx800->ctrl_handler;

	ret = v4l2_async_register_subdev_sensor(&imx800->sd);
	if (ret)
		goto err_ctrls;

	pm_runtime_set_active(imx800->dev);
	pm_runtime_enable(imx800->dev);
	pm_runtime_idle(imx800->dev);
	dev_info(imx800->dev,
		 "IMX800 registered (chip-ID OK; try_stream=%d; link_freq=%llu; rotation=%d CW)\n",
		 try_stream, IMX800_LINK_FREQ_700MHZ, rotation);
	return 0;

err_ctrls:
	v4l2_ctrl_handler_free(&imx800->ctrl_handler);
err_entity:
	media_entity_cleanup(&imx800->sd.entity);
err_power:
	imx800_power_off(imx800->dev);
err_free_gpio:
	if (imx800->reset_own && imx800->reset_gpio)
		gpiod_put(imx800->reset_gpio);
	return ret;
}

static void imx800_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx800 *imx800 = to_imx800(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_ctrl_handler_free(&imx800->ctrl_handler);
	media_entity_cleanup(&sd->entity);
	pm_runtime_disable(imx800->dev);
	if (!pm_runtime_status_suspended(imx800->dev))
		imx800_power_off(imx800->dev);
	pm_runtime_set_suspended(imx800->dev);
	if (imx800->reset_own && imx800->reset_gpio)
		gpiod_put(imx800->reset_gpio);
}

static const struct dev_pm_ops imx800_pm_ops = {
	SET_RUNTIME_PM_OPS(imx800_power_off, imx800_power_on, NULL)
};

static const struct of_device_id imx800_of_match[] = {
	{ .compatible = "sony,imx800" },
	{ }
};
MODULE_DEVICE_TABLE(of, imx800_of_match);

static const struct i2c_device_id imx800_id[] = {
	{ "imx800", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, imx800_id);

static struct i2c_driver imx800_driver = {
	.probe = imx800_probe,
	.remove = imx800_remove,
	.id_table = imx800_id,
	.driver = {
		.name = "imx800",
		.pm = &imx800_pm_ops,
		.of_match_table = imx800_of_match,
	},
};

static int imx800_force_attach(struct device *dev, void *data)
{
	struct i2c_client *client = i2c_verify_client(dev);

	if (!client || !client->dev.of_node)
		return 0;
	if (!of_device_is_compatible(client->dev.of_node, "sony,imx800"))
		return 0;
	if (client->dev.driver)
		return 0;
	pr_info("imx800: force-attach %s\n", dev_name(dev));
	device_driver_attach(&imx800_driver.driver, dev);
	return 0;
}

static int __init imx800_init(void)
{
	int ret;

	pr_info("imx800: init xclr=%d\n", xclr_gpio);
	ret = i2c_add_driver(&imx800_driver);
	if (ret)
		return ret;
	bus_for_each_dev(&i2c_bus_type, NULL, NULL, imx800_force_attach);
	return 0;
}

static void __exit imx800_exit(void)
{
	i2c_del_driver(&imx800_driver);
}

module_init(imx800_init);
module_exit(imx800_exit);
MODULE_DESCRIPTION("Sony IMX800 image sensor");
MODULE_AUTHOR("isyourbrainfoss");
MODULE_LICENSE("GPL");
