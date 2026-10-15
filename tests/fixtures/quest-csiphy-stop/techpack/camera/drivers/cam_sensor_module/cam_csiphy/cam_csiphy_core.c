// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

int32_t cam_csiphy_get_instance_offset(
	struct csiphy_device *csiphy_dev,
	int32_t dev_handle)
{
	int32_t i = 0;

	if ((csiphy_dev->acquire_count >
		csiphy_dev->session_max_device_support) ||
		(csiphy_dev->acquire_count < 0)) {
		CAM_ERR(CAM_CSIPHY,
			"Invalid acquire count: %d, Max supported device for session: %u",
			csiphy_dev->acquire_count,
			csiphy_dev->session_max_device_support);
		return -EINVAL;
	}

	for (i = 0; i < csiphy_dev->session_max_device_support; i++) {
		if (dev_handle ==
			csiphy_dev->csiphy_info[i].hdl_data.device_hdl)
			break;
	}

	return i;
}

int32_t cam_csiphy_core_cfg(void *phy_dev,
			void *arg)
{
	struct cam_control   *cmd = (struct cam_control *)arg;
	struct csiphy_device *csiphy_dev = (struct csiphy_device *)phy_dev;
	struct cam_cphy_dphy_status_reg_params_t *status_reg_ptr;
	struct csiphy_reg_parms_t *csiphy_reg;
	struct cam_hw_soc_info *soc_info;
	uint32_t      cphy_trio_status;
	void __iomem *csiphybase;
	int32_t              rc = 0;
	uint32_t             i;

	if (!csiphy_dev || !cmd) {
		CAM_ERR(CAM_CSIPHY, "Invalid input args");
		return -EINVAL;
	}

	if (cmd->handle_type != CAM_HANDLE_USER_POINTER) {
		CAM_ERR(CAM_CSIPHY, "Invalid handle type: %d",
			cmd->handle_type);
		return -EINVAL;
	}

	soc_info = &csiphy_dev->soc_info;
	if (!soc_info) {
		CAM_ERR(CAM_CSIPHY, "Null Soc_info");
		return -EINVAL;
	}

	if (!g_phy_data[soc_info->index].base_address) {
		CAM_ERR(CAM_CSIPHY, "CSIPHY hw is not avaialble at index: %d",
			soc_info->index);
		return -EINVAL;
	}

	csiphybase = soc_info->reg_map[0].mem_base;
	csiphy_reg = &csiphy_dev->ctrl_reg->csiphy_reg;
	status_reg_ptr = csiphy_reg->status_reg_params;
	CAM_DBG(CAM_CSIPHY, "Opcode received: %d", cmd->op_code);
	mutex_lock(&csiphy_dev->mutex);
	switch (cmd->op_code) {

	case CAM_STOP_DEV: {
		int32_t offset, rc = 0;
		struct cam_start_stop_dev_cmd config;

		rc = copy_from_user(&config, (void __user *)cmd->handle,
					sizeof(config));
		if (rc < 0) {
			CAM_ERR(CAM_CSIPHY, "Failed copying from User");
			goto release_mutex;
		}

		if (csiphy_dev->csiphy_state != CAM_CSIPHY_START) {
			CAM_ERR(CAM_CSIPHY, "Csiphy:%d Not in right state to stop : %d",
				soc_info->index, csiphy_dev->csiphy_state);
			goto release_mutex;
		}

		offset = cam_csiphy_get_instance_offset(csiphy_dev,
			config.dev_handle);
		if (offset < 0 ||
			offset >= csiphy_dev->session_max_device_support) {
			CAM_ERR(CAM_CSIPHY, "Index is invalid: %d", offset);
			goto release_mutex;
		}

		if (--csiphy_dev->start_dev_count) {
			if (csiphy_dev->is_aggregator_rx) {
				CAM_INFO(CAM_CSIPHY,
					"CAM_STOP_PHYDEV: %d dev_cnt: %u, slot: %d",
					soc_info->index,
					csiphy_dev->start_dev_count,
					offset);
				goto release_mutex;
			}

			if (csiphy_dev->csiphy_info[offset].secure_mode)
				cam_csiphy_program_secure_mode(
					csiphy_dev,
					CAM_SECURE_MODE_NON_SECURE, offset, false);

			csiphy_dev->csiphy_info[offset].secure_mode =
				CAM_SECURE_MODE_NON_SECURE;
			csiphy_dev->csiphy_info[offset].csiphy_cpas_cp_reg_mask
				= 0;

			cam_csiphy_update_lane(csiphy_dev, offset, false);

			CAM_DBG(CAM_CSIPHY,
				"CAM_STOP_PHYDEV: %d, Type: %s, dev_cnt: %u, slot: %d, Datarate: %llu, Settletime: %llu",
				soc_info->index,
				g_phy_data[soc_info->index].is_3phase ? "CPHY" : "DPHY",
				csiphy_dev->start_dev_count,
				offset,
				csiphy_dev->csiphy_info[offset].data_rate,
				csiphy_dev->csiphy_info[offset].settle_time);

			goto release_mutex;
		}

		if (csiphy_dev->csiphy_info[offset].secure_mode)
			cam_csiphy_program_secure_mode(
				csiphy_dev,
				CAM_SECURE_MODE_NON_SECURE, offset, false);

		csiphy_dev->csiphy_info[offset].secure_mode =
			CAM_SECURE_MODE_NON_SECURE;

		csiphy_dev->csiphy_info[offset].csiphy_cpas_cp_reg_mask = 0x0;

		if (csiphy_dev->prgm_cmn_reg_across_csiphy) {
			mutex_lock(&active_csiphy_cnt_mutex);
			active_csiphy_hw_cnt--;
			mutex_unlock(&active_csiphy_cnt_mutex);

			cam_csiphy_prgm_cmn_data(csiphy_dev, true);
		}

		if (csiphy_dev->preamble_enable)
			__cam_csiphy_get_preamble_status(csiphy_dev, offset);

		rc = cam_csiphy_disable_hw(csiphy_dev);
		if (rc < 0)
			CAM_ERR(CAM_CSIPHY, "Failed in csiphy release");

		if (cam_csiphy_cpas_ops(csiphy_dev->cpas_handle, false)) {
			CAM_ERR(CAM_CSIPHY, "Failed in de-voting CPAS");
			rc = -EFAULT;
		}

		csiphy_dev->csiphy_state = CAM_CSIPHY_ACQUIRE;

		CAM_DBG(CAM_CSIPHY,
			"CAM_STOP_PHYDEV: %u, Type: %s, slot: %d, Datarate: %llu, Settletime: %llu",
			soc_info->index,
			g_phy_data[soc_info->index].is_3phase ? "CPHY" : "DPHY",
			offset,
			csiphy_dev->csiphy_info[offset].data_rate,
			csiphy_dev->csiphy_info[offset].settle_time);
	}
		break;


	default:
		CAM_ERR(CAM_CSIPHY, "Invalid Opcode: %d", cmd->op_code);
		rc = -EINVAL;
		goto release_mutex;
	}

	mutex_unlock(&csiphy_dev->mutex);
	return rc;


release_mutex:
	mutex_unlock(&csiphy_dev->mutex);

	return rc;
}
