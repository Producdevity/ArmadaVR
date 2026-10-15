// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018-2021, The Linux Foundation. All rights reserved.
 */

int cam_ife_csid_stop(void *hw_priv,
	void *stop_args, uint32_t arg_size)
{
	int rc = 0;
	struct cam_ife_csid_hw               *csid_hw;
	struct cam_hw_info                   *csid_hw_info;
	struct cam_isp_resource_node         *res;
	struct cam_csid_hw_stop_args         *csid_stop;
	uint32_t  i;
	uint32_t res_mask = 0;
	unsigned long flags;

	if (!hw_priv || !stop_args ||
		(arg_size != sizeof(struct cam_csid_hw_stop_args))) {
		CAM_ERR(CAM_ISP, "CSID: Invalid args");
		return -EINVAL;
	}
	csid_stop = (struct cam_csid_hw_stop_args  *) stop_args;

	if (!csid_stop->num_res) {
		CAM_ERR(CAM_ISP, "CSID: Invalid args");
		return -EINVAL;
	}

	csid_hw_info = (struct cam_hw_info  *)hw_priv;
	csid_hw = (struct cam_ife_csid_hw   *)csid_hw_info->core_info;
	CAM_DBG(CAM_ISP, "CSID:%d num_res %d",
		csid_hw->hw_intf->hw_idx,
		csid_stop->num_res);

	spin_lock_irqsave(&csid_hw->lock_state, flags);
	csid_hw->device_enabled = 0;
	cam_ife_csid_csi2_irq_ctrl(csid_hw, false);
	spin_unlock_irqrestore(&csid_hw->lock_state, flags);

	/* Stop the resource first */
	for (i = 0; i < csid_stop->num_res; i++) {
		res = csid_stop->node_res[i];
		CAM_DBG(CAM_ISP, "CSID:%d res_type %d res_id %d",
			csid_hw->hw_intf->hw_idx,
			res->res_type, res->res_id);
		switch (res->res_type) {
		case CAM_ISP_RESOURCE_CID:
			if (csid_hw->res_type == CAM_ISP_IFE_IN_RES_TPG)
				rc = cam_ife_csid_tpg_stop(csid_hw, res);
			break;
		case CAM_ISP_RESOURCE_PIX_PATH:
			res_mask |= (1 << res->res_id);
			if (res->res_id == CAM_IFE_PIX_PATH_RES_IPP ||
				res->res_id == CAM_IFE_PIX_PATH_RES_PPP) {
				rc = cam_ife_csid_disable_pxl_path(csid_hw,
					res, csid_stop->stop_cmd);
			} else if (res->res_id == CAM_IFE_PIX_PATH_RES_RDI_0 ||
				res->res_id == CAM_IFE_PIX_PATH_RES_RDI_1 ||
				res->res_id == CAM_IFE_PIX_PATH_RES_RDI_2 ||
				res->res_id == CAM_IFE_PIX_PATH_RES_RDI_3) {
				rc = cam_ife_csid_disable_rdi_path(csid_hw,
					res, csid_stop->stop_cmd);
			} else if (res->res_id == CAM_IFE_PIX_PATH_RES_UDI_0 ||
			res->res_id == CAM_IFE_PIX_PATH_RES_UDI_1 ||
			res->res_id == CAM_IFE_PIX_PATH_RES_UDI_2) {
				rc = cam_ife_csid_disable_udi_path(csid_hw,
					res, csid_stop->stop_cmd);
			} else {
				CAM_ERR(CAM_ISP, "Invalid res_id: %u",
					res->res_id);
				return -EINVAL;
			}

			break;
		default:
			CAM_ERR(CAM_ISP, "CSID:%d Invalid res type%d",
				csid_hw->hw_intf->hw_idx,
				res->res_type);
			break;
		}
	}

	if (res_mask)
		rc = cam_ife_csid_poll_stop_status(csid_hw, res_mask);

	for (i = 0; i < csid_stop->num_res; i++) {
		res = csid_stop->node_res[i];
		res->res_state = CAM_ISP_RESOURCE_STATE_INIT_HW;
	}

	CAM_DBG(CAM_ISP,  "%s: Exit\n", __func__);

	return rc;

}
