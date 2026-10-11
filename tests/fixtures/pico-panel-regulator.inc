// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2019, The Linux Foundation. All rights reserved.
 */

int dsi_pwr_enable_regulator(struct dsi_regulator_info *regs, bool enable)
{
	int rc = 0;

	if (regs->count == 0) {
		DSI_DEBUG("No valid regulators to enable\n");
		return 0;
	}

	if (!regs->vregs) {
		DSI_ERR("Invalid params\n");
		return -EINVAL;
	}

	if (enable) {
		if (regs->refcount == 0) {
			rc = dsi_pwr_enable_vregs(regs, true);
			if (rc)
				DSI_ERR("failed to enable regulators\n");
		}
		regs->refcount++;
	} else {
		if (regs->refcount == 0) {
			DSI_ERR("Unbalanced regulator off:%s\n",
					regs->vregs->vreg_name);
		} else {
			regs->refcount--;
			if (regs->refcount == 0) {
				rc = dsi_pwr_enable_vregs(regs, false);
				if (rc)
					DSI_ERR("failed to disable vregs\n");
			}
		}
	}

	return rc;
}
