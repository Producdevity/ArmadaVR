/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2016-2019, 2021, The Linux Foundation. All rights reserved.
 *
 * GPIO lifecycle from ByteDance neo3-kernel 9cacc1c1356dd32a048f4cb13bc84af402bd3326,
 * techpack/display/msm/dsi/dsi_panel.c.
 */
static int dsi_panel_gpio_request(struct dsi_panel *panel)
{
	int rc = 0;
	struct dsi_panel_reset_config *r_config = &panel->reset_config;

	if (gpio_is_valid(r_config->reset_gpio)) {
		rc = gpio_request(r_config->reset_gpio, "reset_gpio");
		if (rc) {
			DSI_ERR("request for reset_gpio failed, rc=%d\n", rc);
			goto error;
		}
	}

	if (gpio_is_valid(r_config->disp_en_gpio)) {
		rc = gpio_request(r_config->disp_en_gpio, "disp_en_gpio");
		if (rc) {
			DSI_ERR("request for disp_en_gpio failed, rc=%d\n", rc);
			goto error_release_reset;
		}
	}

	if (gpio_is_valid(panel->bl_config.en_gpio)) {
		rc = gpio_request(panel->bl_config.en_gpio, "bklt_en_gpio");
		if (rc) {
			DSI_ERR("request for bklt_en_gpio failed, rc=%d\n", rc);
			goto error_release_disp_en;
		}
	}

	if (gpio_is_valid(r_config->lcd_mode_sel_gpio)) {
		rc = gpio_request(r_config->lcd_mode_sel_gpio, "mode_gpio");
		if (rc) {
			DSI_ERR("request for mode_gpio failed, rc=%d\n", rc);
			goto error_release_mode_sel;
		}
	}
	/*Added by gavin.zhao for sx2130 lcd vio enable begin*/
	if (gpio_is_valid(panel->panel_vio_gpio)) {
		rc = gpio_request(panel->panel_vio_gpio, "panel_vio_gpio");
		if (rc) {
			DSI_WARN("request for panel_vio_gpio failed, rc=%d\n",
				 rc);
			goto error_release_vio_enable;
		}
	}
	/*Added by gavin.zhao for sx2130 lcd vio enable end*/
	/*Added by gavin.zhao for sx2130 lcd blkd enable begin*/
	if (gpio_is_valid(panel->panel_blk_gpio)) {
		rc = gpio_request(panel->panel_blk_gpio, "panel_blk_gpio");
		if (rc) {
			DSI_WARN("request for panel_blk_gpio failed, rc=%d\n",
				 rc);
			goto error_release_vio_enable;
		}
	}
	/*Added by gavin.zhao for sx2130 lcd blk enable end*/


	if (gpio_is_valid(panel->panel_test_gpio)) {
		rc = gpio_request(panel->panel_test_gpio, "panel_test_gpio");
		if (rc) {
			DSI_WARN("request for panel_test_gpio failed, rc=%d\n",
				 rc);
			panel->panel_test_gpio = -1;
			rc = 0;
		}
	}

	goto error;
	/*Added by gavin.zhao for sx2130 lcd vio enable begin*/
error_release_vio_enable:
	if (gpio_is_valid(r_config->lcd_mode_sel_gpio))
		gpio_free(r_config->lcd_mode_sel_gpio);
	/*Added by gavin.zhao for sx2130 lcd vio enable end*/

error_release_mode_sel:
	if (gpio_is_valid(panel->bl_config.en_gpio))
		gpio_free(panel->bl_config.en_gpio);
error_release_disp_en:
	if (gpio_is_valid(r_config->disp_en_gpio))
		gpio_free(r_config->disp_en_gpio);
error_release_reset:
	if (gpio_is_valid(r_config->reset_gpio))
		gpio_free(r_config->reset_gpio);
error:
	return rc;
}

static int dsi_panel_gpio_release(struct dsi_panel *panel)
{
	int rc = 0;
	struct dsi_panel_reset_config *r_config = &panel->reset_config;

	if (gpio_is_valid(r_config->reset_gpio))
		gpio_free(r_config->reset_gpio);

	if (gpio_is_valid(r_config->disp_en_gpio))
		gpio_free(r_config->disp_en_gpio);

	if (gpio_is_valid(panel->bl_config.en_gpio))
		gpio_free(panel->bl_config.en_gpio);

	if (gpio_is_valid(panel->reset_config.lcd_mode_sel_gpio))
		gpio_free(panel->reset_config.lcd_mode_sel_gpio);

	if (gpio_is_valid(panel->panel_test_gpio))
		gpio_free(panel->panel_test_gpio);

	/*Added by gavin.zhao for sx2130 lcd vio enable begin*/
	if (gpio_is_valid(panel->panel_vio_gpio))
		gpio_free(panel->panel_vio_gpio);
	/*Added by gavin.zhao for sx2130 lcd vio enable end*/
	/*Added by gavin.zhao for sx2130 lcd blk enable begin*/
	if (gpio_is_valid(panel->panel_blk_gpio))
		gpio_free(panel->panel_blk_gpio);
	/*Added by gavin.zhao for sx2130 lcd blk enable end*/

	return rc;
}
