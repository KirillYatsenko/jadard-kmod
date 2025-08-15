// SPDX-License-Identifier: GPL-2.0+
/*
 * Author:
 * - Kirill Yatsenko <kiriyatsenko@gmail.com>
 */

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>

#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

struct jadard;

struct jadard_panel_desc {
	const struct drm_display_mode mode;
	unsigned int lanes;
	enum mipi_dsi_pixel_format format;
	int (*init)(struct jadard *jadard);
	bool lp11_before_reset;
	bool reset_before_power_off_vcioo;
	unsigned int vcioo_to_lp11_delay_ms;
	unsigned int lp11_to_reset_delay_ms;
	unsigned int backlight_off_to_display_off_delay_ms;
	unsigned int display_off_to_enter_sleep_delay_ms;
	unsigned int enter_sleep_to_reset_down_delay_ms;
};

struct jadard {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	const struct jadard_panel_desc *desc;
	enum drm_panel_orientation orientation;
	struct gpio_desc *vdd;
	struct gpio_desc *vccio;
	struct gpio_desc *reset;
	struct gpio_desc *dbg;
};

#define JD9365DA_DCS_SWITCH_PAGE	0xe0

#define jd9365da_switch_page(dsi_ctx, page) \
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, JD9365DA_DCS_SWITCH_PAGE, (page))

static inline struct jadard *panel_to_jadard(struct drm_panel *panel)
{
	return container_of(panel, struct jadard, panel);
}

static int jadard_disable(struct drm_panel *panel)
{
	struct jadard *jadard = panel_to_jadard(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = jadard->dsi };

	if (jadard->desc->backlight_off_to_display_off_delay_ms)
		mipi_dsi_msleep(&dsi_ctx, jadard->desc->backlight_off_to_display_off_delay_ms);

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);

	if (jadard->desc->display_off_to_enter_sleep_delay_ms)
		mipi_dsi_msleep(&dsi_ctx, jadard->desc->display_off_to_enter_sleep_delay_ms);

	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);

	if (jadard->desc->enter_sleep_to_reset_down_delay_ms)
		mipi_dsi_msleep(&dsi_ctx, jadard->desc->enter_sleep_to_reset_down_delay_ms);

	return dsi_ctx.accum_err;
}

static int jadard_prepare(struct drm_panel *panel)
{
	struct jadard *jadard = panel_to_jadard(panel);
	int ret;

	gpiod_set_value(jadard->vccio, 1);
	gpiod_set_value(jadard->vdd, 1);

	if (jadard->desc->vcioo_to_lp11_delay_ms)
		msleep(jadard->desc->vcioo_to_lp11_delay_ms);

	if (jadard->desc->lp11_before_reset) {
		ret = mipi_dsi_dcs_nop(jadard->dsi);
		if (ret)
			return ret;
	}

	if (jadard->desc->lp11_to_reset_delay_ms)
		msleep(jadard->desc->lp11_to_reset_delay_ms);

	gpiod_set_value(jadard->reset, 0);
	msleep(5);

	gpiod_set_value(jadard->reset, 1);
	msleep(10);

	gpiod_set_value(jadard->reset, 0);
	msleep(130);

	ret = jadard->desc->init(jadard);
	if (ret)
		return ret;

	return 0;
}

static int jadard_unprepare(struct drm_panel *panel)
{
	struct jadard *jadard = panel_to_jadard(panel);

	gpiod_set_value(jadard->reset, 0);
	msleep(120);

	if (jadard->desc->reset_before_power_off_vcioo) {
		gpiod_set_value(jadard->reset, 1);

		usleep_range(1000, 2000);
	}

	gpiod_set_value(jadard->vdd, 0);
	gpiod_set_value(jadard->vccio, 0);

	return 0;
}

static int jadard_get_modes(struct drm_panel *panel,
			    struct drm_connector *connector)
{
	struct jadard *jadard = panel_to_jadard(panel);
	const struct drm_display_mode *desc_mode = &jadard->desc->mode;
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, desc_mode);
	if (!mode) {
		DRM_DEV_ERROR(&jadard->dsi->dev, "failed to add mode %ux%ux@%u\n",
			      desc_mode->hdisplay, desc_mode->vdisplay,
			      drm_mode_vrefresh(desc_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	return 1;
}

static enum drm_panel_orientation jadard_panel_get_orientation(struct drm_panel *panel)
{
	struct jadard *jadard = panel_to_jadard(panel);

	return jadard->orientation;
}

static int complex_dbg_pattern=0;
module_param(complex_dbg_pattern,int,0660);

static int shenzen_z34014_p30_365t_y1_init_cmds(struct jadard *jadard)
{
	pr_info("Jadard init start sending\n");

	pr_info("Triggering DBG GPIO for testing\n");
	gpiod_set_value(jadard->dbg, 1);
	usleep_range(1000, 2000);
	gpiod_set_value(jadard->dbg, 0);

	// In case we won't see communication after above gpio trigger,
	// to make sure we capturing right packet
	pr_info("Jadard complex_dbg_pattern: %d\n", complex_dbg_pattern);
	if (complex_dbg_pattern) {
		pr_info("Jadard execute extra gpio triggering\n");
		usleep_range(1000, 2000);
		gpiod_set_value(jadard->dbg, 1);
		usleep_range(1000, 2000);
		gpiod_set_value(jadard->dbg, 0);
	}

	/*
	 * https://regexr.com/
	 *
	 * Used regex to convert for manufacturer's init code
	 *	{(0x..),.[1-9]+,.{(.+)}},
	 *
	 * With replaced value:
	 *	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, $1, $2);
	 *
	 * And then run ClangFormat
	 *
	 */

	struct mipi_dsi_multi_context dsi_ctx = { .dsi = jadard->dsi };

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xDF, 0x90, 0x69, 0xF9);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xDE, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xCC, 0x31);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xB2, 0x01, 0x23, 0x60, 0x88,
				     0x24, 0x5A, 0x07);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xBB, 0x02, 0x1A, 0x33, 0x5A,
				     0x3C, 0x44, 0x44);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xBD, 0x00, 0xD0, 0x00);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xBF, 0x50, 0x3C, 0x33, 0xC3);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xC0, 0x01, 0xAD, 0x01, 0xAD);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xCB, 0x7F, 0x7A, 0x75, 0x6C,
				     0x63, 0x64, 0x57, 0x5C, 0x46, 0x5C, 0x57,
				     0x53, 0x6B, 0x54, 0x56, 0x44, 0x3E, 0x2F,
				     0x1D, 0x14, 0x10, 0x7F, 0x7A, 0x75, 0x6C,
				     0x63, 0x64, 0x57, 0x5C, 0x46, 0x5C, 0x57,
				     0x53, 0x6B, 0x54, 0x56, 0x44, 0x3E, 0x2F,
				     0x1D, 0x14, 0x10, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xC3, 0x3B, 0x01, 0x00, 0x03,
				     0x08, 0x08, 0x4C, 0x05, 0x4E, 0x05, 0x4E,
				     0x01, 0x48, 0x01, 0x48, 0x01, 0x48, 0x06,
				     0x4A, 0x06, 0x09, 0x06, 0x09, 0x06, 0x09);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xC4, 0x01, 0x00, 0x03, 0x08,
				     0x08, 0x4C, 0x05, 0x4E, 0x05, 0x4E, 0x01,
				     0x48, 0x01, 0x48, 0x01, 0x48, 0x06, 0x4A,
				     0x06, 0x09, 0x06, 0x09, 0x06, 0x09);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xC5, 0x03, 0x03, 0x08, 0x08,
				     0x4C, 0x05, 0x4E, 0x05, 0x4E, 0x01, 0x48,
				     0x01, 0x48, 0x01, 0x48, 0x06, 0x4A, 0x06,
				     0x09, 0x06, 0x09, 0x06, 0x09);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xC6, 0x00, 0x59, 0x00, 0xB4,
				     0x00, 0x13, 0x28, 0x82, 0x00, 0x00, 0x00,
				     0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x01, 0x00, 0x00, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xC8, 0x2B, 0x1C, 0x78);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xCD, 0x06, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xCE, 0x00, 0x00, 0x00, 0x0C,
				     0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xCF, 0x00, 0x00, 0x00, 0x30,
				     0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF,
				     0xFF, 0xFF, 0xFF, 0xFF, 0x3F);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xD0, 0x00, 0x1F, 0x1F, 0x11,
				     0x24, 0x24, 0x0B, 0x09, 0x07, 0x05, 0x01,
				     0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F,
				     0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xD1, 0x00, 0x1F, 0x1F, 0x10,
				     0x24, 0x24, 0x0A, 0x08, 0x06, 0x04, 0x00,
				     0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F,
				     0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xD2, 0x00, 0x1F, 0x1F, 0x00,
				     0x24, 0x24, 0x08, 0x0A, 0x04, 0x06, 0x10,
				     0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F,
				     0x1F, 0x1F, 0x1F, 0x1F, 0x1F);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xD3, 0x00, 0x1F, 0x1F, 0x00,
				     0x24, 0x24, 0x09, 0x0B, 0x05, 0x07, 0x11,
				     0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F,
				     0x1F, 0x1F, 0x1F, 0x1F, 0x1F);

	mipi_dsi_dcs_write_seq_multi(
		&dsi_ctx, 0xD4, 0x00, 0x20, 0x0C, 0x00, 0x0A, 0x00, 0x0C, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06,
		0x03, 0x03, 0x00, 0x81, 0x04, 0x4C, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x80, 0x09, 0x00, 0x0A, 0x06, 0x55, 0x06,
		0x0D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00);

	mipi_dsi_dcs_write_seq_multi(
		&dsi_ctx, 0xD5, 0x02, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0xE0, 0x00, 0x00, 0x00, 0x07, 0x32, 0x5A, 0x00, 0x00,
		0x05, 0x00, 0x01, 0x00, 0x30, 0x74, 0x00, 0x0E, 0x00, 0x08,
		0x00, 0x71, 0x20, 0x04, 0x10, 0x04, 0x06, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x1F,
		0xFF, 0x00, 0x00, 0x00, 0x1F, 0xFF, 0x00, 0xFF, 0xFF, 0xFF,
		0xFF, 0xFF, 0xFF, 0x00);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xD7, 0x00, 0x34, 0x34, 0x34,
				     0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34,
				     0x34, 0x34, 0x34, 0x34, 0x34, 0x34);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xDE, 0x01);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xB9, 0x00, 0xFF, 0xFF, 0x04);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xC7, 0x1B, 0x14, 0x0E);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xDE, 0x02);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xBB, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x68, 0x69);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xBD, 0x1B);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xC1, 0x00, 0x40, 0x00, 0x02,
				     0x02, 0x02, 0x02, 0x7F, 0x00, 0x00, 0x00,
				     0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xC3, 0x20, 0xFF);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xC4, 0x00, 0x11, 0x07, 0x00,
				     0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xC6, 0x49, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xE5, 0x00, 0xE6, 0xE5, 0x02,
				     0x27, 0x42, 0x27, 0x42, 0x09, 0x04, 0x00,
				     0x40, 0x00, 0x21, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xE6, 0x10, 0x09, 0xAD, 0x00,
				     0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xEC, 0x07, 0x07, 0x40, 0x00,
				     0x22, 0x02, 0x00, 0xFF, 0x08, 0x7C, 0x00,
				     0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xDE, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xD1, 0x00, 0x00, 0x21, 0xFF,
				     0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xDE, 0x00);


	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	mipi_dsi_msleep(&dsi_ctx, 30);

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 10);

	if (dsi_ctx.accum_err)
		pr_err("MIPI init code error!\n");

	pr_info("Jadard init finished\n");

	return dsi_ctx.accum_err;
}

static const struct jadard_panel_desc shenzen_z34014_p30_365t_y1_desc = {
	.mode = {
		.clock	= (480 + 20 + 20 + 40) * (1080 + 180 + 2 + 18) * 60 / 1000,

		// Horizontal timing (from manufacturer)
		.hdisplay = 480,
		.hsync_start = 480 + 20,        // 500 (hdisplay + HFP)
		.hsync_end = 480 + 20 + 20,     // 520 (+ HSYNC width)
		.htotal = 480 + 20 + 20 + 40,   // 560 (+ HBP)

		// Vertical timing (from manufacturer)
		.vdisplay = 1080,
		.vsync_start = 1080 + 180,      // 1260 (vdisplay + VFP)
		.vsync_end = 1080 + 180 + 2,    // 1262 (+ VSYNC width)
		.vtotal = 1080 + 180 + 2 + 18,  // 1280 (+ VBP)

		// Physical dimensions (from manufacturer)
		.width_mm = 42,   // 42.0mm width
		.height_mm = 95,  // 94.5mm height (rounded)
		.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
	},
	.lanes = 2,
	.format = MIPI_DSI_FMT_RGB888,
	.init = shenzen_z34014_p30_365t_y1_init_cmds,
};

static const struct drm_panel_funcs jadard_funcs = {
	.disable = jadard_disable,
	.unprepare = jadard_unprepare,
	.prepare = jadard_prepare,
	.get_modes = jadard_get_modes,
	.get_orientation = jadard_panel_get_orientation,
};

static int jadard_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct jadard_panel_desc *desc;
	struct jadard *jadard;
	int ret;

	pr_info("Jadard driver starting probe!\n");

	jadard = devm_kzalloc(&dsi->dev, sizeof(*jadard), GFP_KERNEL);
	if (!jadard)
		return -ENOMEM;

	desc = of_device_get_match_data(dev);
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_NO_EOT_PACKET | MIPI_DSI_MODE_LPM;
	dsi->format = desc->format;
	dsi->lanes = desc->lanes;

	jadard->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(jadard->reset)) {
		DRM_DEV_ERROR(&dsi->dev, "failed to get our reset GPIO\n");
		return PTR_ERR(jadard->reset);
	}

	/* VDD pin is connected to power regulator enable pin on adapter board */
	jadard->vdd = devm_gpiod_get(dev, "vdd", GPIOD_OUT_LOW);
	if (IS_ERR(jadard->vdd)) {
		DRM_DEV_ERROR(&dsi->dev, "failed to get vdd GPIO\n");
		return PTR_ERR(jadard->vdd);
	}

	/* VCCIO pin is connected to power regulator enable pin on adapter board */
	jadard->vccio = devm_gpiod_get(dev, "vccio", GPIOD_OUT_LOW);
	if (IS_ERR(jadard->vccio)) {
		DRM_DEV_ERROR(&dsi->dev, "failed to get vccio GPIO\n");
		return PTR_ERR(jadard->vccio);
	}

	/* DBG pin is used for osciloscope debugging */
	jadard->dbg = devm_gpiod_get(dev, "dbg", GPIOD_OUT_HIGH);
	if (IS_ERR(jadard->dbg)) {
		DRM_DEV_ERROR(&dsi->dev, "failed to get dbg GPIO\n");
		return PTR_ERR(jadard->dbg);
	}

	drm_panel_init(&jadard->panel, dev, &jadard_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	ret = of_drm_get_panel_orientation(dev->of_node, &jadard->orientation);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get orientation\n");

	ret = drm_panel_of_backlight(&jadard->panel);
	if (ret)
		return ret;

	drm_panel_add(&jadard->panel);

	mipi_dsi_set_drvdata(dsi, jadard);
	jadard->dsi = dsi;
	jadard->desc = desc;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0)
		drm_panel_remove(&jadard->panel);

	return ret;
}

static void jadard_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct jadard *jadard = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&jadard->panel);
}

static const struct of_device_id jadard_of_match[] = {
	{
		.compatible = "shenzen,z34014p30365ty1",
		.data = &shenzen_z34014_p30_365t_y1_desc
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, jadard_of_match);

static struct mipi_dsi_driver jadard_driver = {
	.probe = jadard_dsi_probe,
	.remove = jadard_dsi_remove,
	.driver = {
		.name = "jadard-jd9365tn",
		.of_match_table = jadard_of_match,
	},
};
module_mipi_dsi_driver(jadard_driver);

MODULE_AUTHOR("Kirill Yatsenko <kiriyatsenko@gmail.com>");
MODULE_DESCRIPTION("Jadard JD9365TN WXGA DSI panel");
MODULE_LICENSE("GPL");
