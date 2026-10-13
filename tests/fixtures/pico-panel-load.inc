// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2016-2019, The Linux Foundation. All rights reserved. */

static int dsi_pwr_enable_vregs(struct dsi_regulator_info *regs, bool enable)
{
	int rc = 0, i = 0;
	struct dsi_vreg *vreg;
	int num_of_v = 0;
	u32 pre_on_ms, post_on_ms;
	u32 pre_off_ms, post_off_ms;

	if (enable) {
		for (i = 0; i < regs->count; i++) {
			vreg = &regs->vregs[i];
			pre_on_ms = vreg->pre_on_sleep;
			post_on_ms = vreg->post_on_sleep;

			if (vreg->pre_on_sleep)
				usleep_range((pre_on_ms * 1000),
						(pre_on_ms * 1000) + 10);

			rc = regulator_set_load(vreg->vreg,
						vreg->enable_load);
			if (rc < 0) {
				DSI_ERR("Setting optimum mode failed for %s\n",
				       vreg->vreg_name);
				goto error;
			}
			num_of_v = regulator_count_voltages(vreg->vreg);
			if (num_of_v > 0) {
				rc = regulator_set_voltage(vreg->vreg,
							   vreg->min_voltage,
							   vreg->max_voltage);
				if (rc) {
					DSI_ERR("Set voltage(%s) fail, rc=%d\n",
						 vreg->vreg_name, rc);
					goto error_disable_opt_mode;
				}
			}

			rc = regulator_enable(vreg->vreg);
			if (rc) {
				DSI_ERR("enable failed for %s, rc=%d\n",
				       vreg->vreg_name, rc);
				goto error_disable_voltage;
			}

			if (vreg->post_on_sleep)
				usleep_range((post_on_ms * 1000),
						(post_on_ms * 1000) + 10);
		}
	} else {
		for (i = (regs->count - 1); i >= 0; i--) {
			vreg = &regs->vregs[i];
			pre_off_ms = vreg->pre_off_sleep;
			post_off_ms = vreg->post_off_sleep;

			if (pre_off_ms)
				usleep_range((pre_off_ms * 1000),
						(pre_off_ms * 1000) + 10);

			if (regs->vregs[i].off_min_voltage)
				(void)regulator_set_voltage(regs->vregs[i].vreg,
						regs->vregs[i].off_min_voltage,
						regs->vregs[i].max_voltage);

			(void)regulator_set_load(regs->vregs[i].vreg,
						regs->vregs[i].disable_load);
			(void)regulator_disable(regs->vregs[i].vreg);

			if (post_off_ms)
				usleep_range((post_off_ms * 1000),
						(post_off_ms * 1000) + 10);
		}
	}

	return 0;
error_disable_opt_mode:
	(void)regulator_set_load(regs->vregs[i].vreg,
				 regs->vregs[i].disable_load);

error_disable_voltage:
	if (num_of_v > 0)
		(void)regulator_set_voltage(regs->vregs[i].vreg,
					    0, regs->vregs[i].max_voltage);
error:
	for (i--; i >= 0; i--) {
		vreg = &regs->vregs[i];
		pre_off_ms = vreg->pre_off_sleep;
		post_off_ms = vreg->post_off_sleep;

		if (pre_off_ms)
			usleep_range((pre_off_ms * 1000),
					(pre_off_ms * 1000) + 10);

		(void)regulator_set_load(regs->vregs[i].vreg,
					 regs->vregs[i].disable_load);

		num_of_v = regulator_count_voltages(regs->vregs[i].vreg);
		if (num_of_v > 0)
			(void)regulator_set_voltage(regs->vregs[i].vreg,
					    0, regs->vregs[i].max_voltage);

		(void)regulator_disable(regs->vregs[i].vreg);

		if (post_off_ms)
			usleep_range((post_off_ms * 1000),
					(post_off_ms * 1000) + 10);
	}

	return rc;
}
