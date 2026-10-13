// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2021, The Linux Foundation. All rights reserved.
 */

int dsi_display_cont_splash_config(void *dsi_display)
{
	struct dsi_display *display = dsi_display;
	int rc = 0;

	/* Vote for gdsc required to read register address space */
	if (!display) {
		DSI_ERR("invalid input display param\n");
		return -EINVAL;
	}

	rc = pm_runtime_get_sync(display->drm_dev->dev);
	if (rc < 0) {
		DSI_ERR("failed to vote gdsc for continuous splash, rc=%d\n",
							rc);
		return rc;
	}

	mutex_lock(&display->display_lock);

	display->is_cont_splash_enabled = true;

	/* Update splash status for clock manager */
	dsi_display_clk_mngr_update_splash_status(display->clk_mngr,
				display->is_cont_splash_enabled);

	/* Set up ctrl isr before enabling core clk */
	dsi_display_ctrl_isr_configure(display, true);

	/* Vote for Core clk and link clk. Votes on ctrl and phy
	 * regulator are inplicit from  pre clk on callback
	 */
	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_ON);
	if (rc) {
		DSI_ERR("[%s] failed to enable DSI link clocks, rc=%d\n",
		       display->name, rc);
		goto clk_manager_update;
	}

	/* Vote on panel regulator will be removed during suspend path */
	rc = dsi_pwr_enable_regulator(&display->panel->power_info, true);
	if (rc) {
		DSI_ERR("[%s] failed to enable vregs, rc=%d\n",
				display->panel->name, rc);
		goto clks_disabled;
	}

	dsi_config_host_engine_state_for_cont_splash(display);
	mutex_unlock(&display->display_lock);

	/* Set the current brightness level */
	dsi_panel_bl_handoff(display->panel);

	return rc;

clks_disabled:
	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_OFF);

clk_manager_update:
	dsi_display_ctrl_isr_configure(display, false);
	/* Update splash status for clock manager */
	dsi_display_clk_mngr_update_splash_status(display->clk_mngr,
				false);
	pm_runtime_put_sync(display->drm_dev->dev);
	display->is_cont_splash_enabled = false;
	mutex_unlock(&display->display_lock);
	return rc;
}
