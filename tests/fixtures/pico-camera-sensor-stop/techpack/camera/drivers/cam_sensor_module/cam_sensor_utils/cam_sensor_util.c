// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2020, The Linux Foundation. All rights reserved.
 */

int cam_sensor_util_power_down(struct cam_sensor_power_ctrl_t *ctrl,
		struct cam_hw_soc_info *soc_info)
{
	int index = 0, ret = 0, num_vreg = 0, i;
	struct cam_sensor_power_setting *pd = NULL;
	struct cam_sensor_power_setting *ps = NULL;
	struct msm_camera_gpio_num_info *gpio_num_info = NULL;

	CAM_INFO(CAM_SENSOR, "Enter");
	if (!ctrl || !soc_info) {
		CAM_ERR(CAM_SENSOR, "failed ctrl %pK",  ctrl);
		return -EINVAL;
	}

	gpio_num_info = ctrl->gpio_num_info;
	num_vreg = soc_info->num_rgltr;

	if ((num_vreg <= 0) || (num_vreg > CAM_SOC_MAX_REGULATOR)) {
		CAM_ERR(CAM_SENSOR, "failed: num_vreg %d", num_vreg);
		return -EINVAL;
	}

	if (ctrl->power_down_setting_size > MAX_POWER_CONFIG) {
		CAM_ERR(CAM_SENSOR, "Invalid: power setting size %d",
			ctrl->power_setting_size);
		return -EINVAL;
	}
	CAM_INFO(CAM_SENSOR, "soc_info.index = %d", soc_info->index);

	for (index = 0; index < ctrl->power_down_setting_size; index++) {
		CAM_DBG(CAM_SENSOR, "power_down_index %d",  index);
		pd = &ctrl->power_down_setting[index];
		if (!pd) {
			CAM_ERR(CAM_SENSOR,
				"Invalid power down settings for index %d",
				index);
			return -EINVAL;
		}

		ps = NULL;
		CAM_DBG(CAM_SENSOR, "seq_type %d",  pd->seq_type);
		switch (pd->seq_type) {
		case SENSOR_MCLK:
			for (i = soc_info->num_clk - 1; i >= 0; i--) {
				cam_soc_util_clk_disable(soc_info->clk[i],
					soc_info->clk_name[i]);
			}

			ret = cam_config_mclk_reg(ctrl, soc_info, index);
			if (ret < 0) {
				CAM_ERR(CAM_SENSOR,
					"config clk reg failed rc: %d", ret);
				continue;
			}
			break;
		case SENSOR_RESET:
		case SENSOR_STANDBY:
		case SENSOR_CUSTOM_GPIO1:
		case SENSOR_CUSTOM_GPIO2:

			if (!gpio_num_info->valid[pd->seq_type])
				continue;

			cam_res_mgr_gpio_set_value(
				gpio_num_info->gpio_num
				[pd->seq_type],
				(int) pd->config_val);

			break;
		case SENSOR_VANA:
		case SENSOR_VDIG:
		case SENSOR_VIO:
		case SENSOR_VAF:
		case SENSOR_VAF_PWDM:
		case SENSOR_CUSTOM_REG1:
		case SENSOR_CUSTOM_REG2:
			if (pd->seq_val == INVALID_VREG)
				break;
		#ifdef NEO25_FOUR_6DOF_CAMERAS_SUPPORT
			CAM_INFO(CAM_SENSOR, "SENSOR_VANA soc_info.index = %d", soc_info->index);
			if((soc_info.index == 2)|| (soc_info.index == 3)||(soc_info.index == 5))
			{
				return 0;
			}
		#endif

			ps = msm_camera_get_power_settings(
				ctrl, pd->seq_type,
				pd->seq_val);
			/*Modify by PICO_Driver sophia.wang for bug55100 : adjust the power sequence for camera start*/
			ret = msm_cam_sensor_handle_reg_gpio(pd->seq_type,
				gpio_num_info, GPIOF_OUT_INIT_LOW);

			if (ret < 0)
				CAM_ERR(CAM_SENSOR,
					"Error disabling VREG GPIO");
			msleep(10);
			/*Modify by PICO_Driver sophia.wang for bug55100 : adjust the power sequence for camera end*/

			if (ps) {
				if (pd->seq_val < num_vreg) {
					CAM_DBG(CAM_SENSOR,
						"Disable Regulator");
					ret =  cam_soc_util_regulator_disable(
					soc_info->rgltr[ps->seq_val],
					soc_info->rgltr_name[ps->seq_val],
					soc_info->rgltr_min_volt[ps->seq_val],
					soc_info->rgltr_max_volt[ps->seq_val],
					soc_info->rgltr_op_mode[ps->seq_val],
					soc_info->rgltr_delay[ps->seq_val]);
					if (ret) {
						CAM_ERR(CAM_SENSOR,
						"Reg: %s disable failed",
						soc_info->rgltr_name[
							ps->seq_val]);
						soc_info->rgltr[ps->seq_val] =
							NULL;
						msm_cam_sensor_handle_reg_gpio(
							pd->seq_type,
							gpio_num_info,
							GPIOF_OUT_INIT_LOW);
						continue;
					}
					ps->data[0] =
						soc_info->rgltr[ps->seq_val];
					regulator_put(
						soc_info->rgltr[ps->seq_val]);
					soc_info->rgltr[ps->seq_val] = NULL;
				} else {
					CAM_ERR(CAM_SENSOR,
						"seq_val:%d > num_vreg: %d",
						 pd->seq_val,
						num_vreg);
				}
			} else
				CAM_ERR(CAM_SENSOR,
					"error in power up/down seq");
			/*Modify by PICO_Driver sophia.wang for bug55100 : adjust the power sequence for camera start*/

			/*ret = msm_cam_sensor_handle_reg_gpio(pd->seq_type,
				gpio_num_info, GPIOF_OUT_INIT_LOW);

			if (ret < 0)
				CAM_ERR(CAM_SENSOR,
					"Error disabling VREG GPIO");*/
			/*Modify by PICO_Driver sophia.wang for bug55100 : adjust the power sequence for camera end*/
			break;
		default:
			CAM_ERR(CAM_SENSOR, "error power seq type %d",
				pd->seq_type);
			break;
		}
		if (pd->delay > 20)
			msleep(pd->delay);
		else if (pd->delay)
			usleep_range(pd->delay * 1000,
				(pd->delay * 1000) + 1000);
	}

	if (ctrl->cam_pinctrl_status) {
		ret = pinctrl_select_state(
				ctrl->pinctrl_info.pinctrl,
				ctrl->pinctrl_info.gpio_state_suspend);
		if (ret)
			CAM_ERR(CAM_SENSOR, "cannot set pin to suspend state");

		devm_pinctrl_put(ctrl->pinctrl_info.pinctrl);
	}

	if (soc_info->use_shared_clk)
		cam_res_mgr_shared_clk_config(false);

	cam_res_mgr_shared_pinctrl_select_state(false);
	cam_res_mgr_shared_pinctrl_put();

	ctrl->cam_pinctrl_status = 0;

	cam_sensor_util_request_gpio_table(soc_info, 0);

	return 0;
}
