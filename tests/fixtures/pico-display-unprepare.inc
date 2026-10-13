/* SPDX-License-Identifier: GPL-2.0-only */

int dsi_display_unprepare(struct dsi_display *display)
{
	int rc = 0;

	if (!display) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	SDE_EVT32(SDE_EVTLOG_FUNC_ENTRY);
	mutex_lock(&display->display_lock);

	rc = dsi_display_wake_up(display);
	if (rc)
		DSI_ERR("[%s] display wake up failed, rc=%d\n",
		       display->name, rc);
	if (!display->poms_pending) {
		rc = dsi_panel_unprepare(display->panel);
		if (rc)
			DSI_ERR("[%s] panel unprepare failed, rc=%d\n",
			       display->name, rc);
	}
	rc = dsi_display_ctrl_host_disable(display);
	if (rc)
		DSI_ERR("[%s] failed to disable DSI host, rc=%d\n",
		       display->name, rc);

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_LINK_CLK, DSI_CLK_OFF);
	if (rc)
		DSI_ERR("[%s] failed to disable Link clocks, rc=%d\n",
		       display->name, rc);

	rc = dsi_display_ctrl_deinit(display);
	if (rc)
		DSI_ERR("[%s] failed to deinit controller, rc=%d\n",
		       display->name, rc);

	if (!display->panel->ulps_suspend_enabled) {
		rc = dsi_display_phy_disable(display);
		if (rc)
			DSI_ERR("[%s] failed to disable DSI PHY, rc=%d\n",
			       display->name, rc);
	}

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_CORE_CLK, DSI_CLK_OFF);
	if (rc)
		DSI_ERR("[%s] failed to disable DSI clocks, rc=%d\n",
		       display->name, rc);

	/* destrory dsi isr set up */
	dsi_display_ctrl_isr_configure(display, false);

	if (!display->poms_pending) {
		rc = dsi_panel_post_unprepare(display->panel);
		if (rc)
			DSI_ERR("[%s] panel post-unprepare failed, rc=%d\n",
			       display->name, rc);
	}

	mutex_unlock(&display->display_lock);

	/* Free up DSI ERROR event callback */
	dsi_display_unregister_error_handler(display);

	SDE_EVT32(SDE_EVTLOG_FUNC_EXIT);
	return rc;
}
