// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2020, The Linux Foundation. All rights reserved.
 */

int cam_soc_util_regulator_disable(struct regulator *rgltr,
	const char *rgltr_name, uint32_t rgltr_min_volt,
	uint32_t rgltr_max_volt, uint32_t rgltr_op_mode,
	uint32_t rgltr_delay_ms)
{
	int32_t rc = 0;

	if (!rgltr) {
		CAM_ERR(CAM_UTIL, "Invalid NULL parameter");
		return -EINVAL;
	}

	rc = regulator_disable(rgltr);
	if (rc) {
		CAM_ERR(CAM_UTIL, "%s regulator disable failed", rgltr_name);
		return rc;
	}

	if (rgltr_delay_ms > 20)
		msleep(rgltr_delay_ms);
	else if (rgltr_delay_ms)
		usleep_range(rgltr_delay_ms * 1000,
			(rgltr_delay_ms * 1000) + 1000);

	if (regulator_count_voltages(rgltr) > 0) {
		regulator_set_load(rgltr, 0);
		regulator_set_voltage(rgltr, 0, rgltr_max_volt);
	}

	return rc;
}


static void cam_soc_util_regulator_disable_default(
	struct cam_hw_soc_info *soc_info)
{
	int j = 0;
	uint32_t num_rgltr = soc_info->num_rgltr;

	for (j = num_rgltr-1; j >= 0; j--) {
		if (soc_info->rgltr_ctrl_support == true) {
			cam_soc_util_regulator_disable(soc_info->rgltr[j],
				soc_info->rgltr_name[j],
				soc_info->rgltr_min_volt[j],
				soc_info->rgltr_max_volt[j],
				soc_info->rgltr_op_mode[j],
				soc_info->rgltr_delay[j]);
		} else {
			if (soc_info->rgltr[j])
				regulator_disable(soc_info->rgltr[j]);
		}
	}
}

int cam_soc_util_disable_platform_resource(struct cam_hw_soc_info *soc_info,
	bool disable_clocks, bool disable_irq)
{
	int rc = 0;

	if (!soc_info)
		return -EINVAL;

	if (disable_irq)
		rc |= cam_soc_util_irq_disable(soc_info);

	if (disable_clocks)
		cam_soc_util_clk_disable_default(soc_info);

	cam_soc_util_regulator_disable_default(soc_info);

	if (soc_info->pinctrl_info.pinctrl &&
		soc_info->pinctrl_info.gpio_state_suspend)
		rc = pinctrl_select_state(soc_info->pinctrl_info.pinctrl,
			soc_info->pinctrl_info.gpio_state_suspend);

	return rc;
}
