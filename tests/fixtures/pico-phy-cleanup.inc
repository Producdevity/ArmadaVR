// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2020, The Linux Foundation. All rights reserved.
 */

int dsi_phy_set_power_state(struct msm_dsi_phy *dsi_phy, bool enable)
{
	int rc = 0;

	if (!dsi_phy) {
		DSI_PHY_ERR(dsi_phy, "Invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&dsi_phy->phy_lock);

	if (enable == dsi_phy->power_state) {
		DSI_PHY_ERR(dsi_phy, "No state change\n");
		goto error;
	}

	if (enable) {
		rc = dsi_pwr_enable_regulator(&dsi_phy->pwr_info.digital, true);
		if (rc) {
			DSI_PHY_ERR(dsi_phy, "failed to enable digital regulator\n");
			goto error;
		}

		if (dsi_phy->dsi_phy_state == DSI_PHY_ENGINE_OFF &&
				dsi_phy->regulator_required) {
			rc = dsi_pwr_enable_regulator(
				&dsi_phy->pwr_info.phy_pwr, true);
			if (rc) {
				DSI_PHY_ERR(dsi_phy, "failed to enable phy power\n");
				(void)dsi_pwr_enable_regulator(
					&dsi_phy->pwr_info.digital, false);
				goto error;
			}
		}
	} else {
		if (dsi_phy->dsi_phy_state == DSI_PHY_ENGINE_OFF &&
				dsi_phy->regulator_required) {
			rc = dsi_pwr_enable_regulator(
				&dsi_phy->pwr_info.phy_pwr, false);
			if (rc) {
				DSI_PHY_ERR(dsi_phy, "failed to enable digital regulator\n");
				goto error;
			}
		}

		rc = dsi_pwr_enable_regulator(&dsi_phy->pwr_info.digital,
					      false);
		if (rc) {
			DSI_PHY_ERR(dsi_phy, "failed to enable phy power\n");
			goto error;
		}
	}

	dsi_phy->power_state = enable;
error:
	mutex_unlock(&dsi_phy->phy_lock);
	return rc;
}
