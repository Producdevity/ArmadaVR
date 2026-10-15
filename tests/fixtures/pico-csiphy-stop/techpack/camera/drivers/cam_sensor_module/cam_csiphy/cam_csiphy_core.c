// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2020, The Linux Foundation. All rights reserved.
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

	for (i = 0; i < csiphy_dev->acquire_count; i++) {
		if (dev_handle ==
			csiphy_dev->csiphy_info[i].hdl_data.device_hdl)
			break;
	}

	return i;
}

int32_t cam_csiphy_core_cfg(void *phy_dev,
			void *arg)
{
	struct csiphy_device *csiphy_dev =
		(struct csiphy_device *)phy_dev;
	struct cam_control   *cmd = (struct cam_control *)arg;
	int32_t              rc = 0;

	if (!csiphy_dev || !cmd) {
		CAM_ERR(CAM_CSIPHY, "Invalid input args");
		return -EINVAL;
	}

	if (cmd->handle_type != CAM_HANDLE_USER_POINTER) {
		CAM_ERR(CAM_CSIPHY, "Invalid handle type: %d",
			cmd->handle_type);
		return -EINVAL;
	}

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
			CAM_ERR(CAM_CSIPHY, "Not in right state to stop : %d",
				csiphy_dev->csiphy_state);
			goto release_mutex;
		}

		offset = cam_csiphy_get_instance_offset(csiphy_dev,
			config.dev_handle);
		if (offset < 0 ||
			offset >= csiphy_dev->session_max_device_support) {
			CAM_ERR(CAM_CSIPHY, "Index is invalid: %d", offset);
			goto release_mutex;
		}

		CAM_INFO(CAM_CSIPHY,
			"STOP_DEV: CSIPHY_IDX: %d, Device_slot: %d, Datarate: %llu, Settletime: %llu",
			csiphy_dev->soc_info.index, offset,
			csiphy_dev->csiphy_info[offset].data_rate,
			csiphy_dev->csiphy_info[offset].settle_time);

		if (--csiphy_dev->start_dev_count) {
			CAM_DBG(CAM_CSIPHY, "Stop Dev ref Cnt: %d",
				csiphy_dev->start_dev_count);
			if (csiphy_dev->csiphy_info[offset].secure_mode)
				cam_csiphy_notify_secure_mode(
					csiphy_dev,
					CAM_SECURE_MODE_NON_SECURE, offset);

			csiphy_dev->csiphy_info[offset].secure_mode =
				CAM_SECURE_MODE_NON_SECURE;
			csiphy_dev->csiphy_info[offset].csiphy_cpas_cp_reg_mask
				= 0;

			cam_csiphy_update_lane(csiphy_dev, offset, false);
			goto release_mutex;
		}

		if (csiphy_dev->csiphy_info[offset].secure_mode)
			cam_csiphy_notify_secure_mode(
				csiphy_dev,
				CAM_SECURE_MODE_NON_SECURE, offset);

		csiphy_dev->csiphy_info[offset].secure_mode =
			CAM_SECURE_MODE_NON_SECURE;

		csiphy_dev->csiphy_info[offset].csiphy_cpas_cp_reg_mask = 0x0;

		rc = cam_csiphy_disable_hw(csiphy_dev);
		if (rc < 0)
			CAM_ERR(CAM_CSIPHY, "Failed in csiphy release");

		rc = cam_cpas_stop(csiphy_dev->cpas_handle);
		if (rc < 0)
			CAM_ERR(CAM_CSIPHY, "de-voting CPAS: %d", rc);

		CAM_DBG(CAM_CSIPHY, "All PHY devices stopped");
		csiphy_dev->csiphy_state = CAM_CSIPHY_ACQUIRE;
	}
		break;


	default:
		CAM_ERR(CAM_CSIPHY, "Invalid Opcode: %d", cmd->op_code);
		rc = -EINVAL;
		goto release_mutex;
	}

release_mutex:
	mutex_unlock(&csiphy_dev->mutex);

	return rc;
}
