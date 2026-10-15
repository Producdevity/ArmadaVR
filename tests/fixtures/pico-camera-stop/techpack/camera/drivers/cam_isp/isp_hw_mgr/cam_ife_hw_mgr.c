// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2021, The Linux Foundation. All rights reserved.
 */

static void cam_ife_hw_mgr_stop_hw_res(
	struct cam_ife_hw_mgr_res   *isp_hw_res)
{
	int i;
	struct cam_hw_intf      *hw_intf;
	uint32_t dummy_args;

	for (i = 0; i < CAM_ISP_HW_SPLIT_MAX; i++) {
		if (!isp_hw_res->hw_res[i])
			continue;
		hw_intf = isp_hw_res->hw_res[i]->hw_intf;

		if (isp_hw_res->hw_res[i]->res_state !=
			CAM_ISP_RESOURCE_STATE_STREAMING)
			continue;

		if (hw_intf->hw_ops.stop)
			hw_intf->hw_ops.stop(hw_intf->hw_priv,
				isp_hw_res->hw_res[i],
				sizeof(struct cam_isp_resource_node));
		else
			CAM_ERR(CAM_ISP, "stop null");
		if (hw_intf->hw_ops.process_cmd &&
			isp_hw_res->res_type == CAM_IFE_HW_MGR_RES_IFE_OUT) {
			hw_intf->hw_ops.process_cmd(hw_intf->hw_priv,
				CAM_ISP_HW_CMD_STOP_BUS_ERR_IRQ,
				&dummy_args, sizeof(dummy_args));
		}
	}
}

static int cam_ife_mgr_csid_change_halt_mode(struct list_head  *halt_list,
	uint32_t  base_idx, enum cam_ife_csid_halt_mode halt_mode)
{
	struct cam_ife_hw_mgr_res      *hw_mgr_res;
	struct cam_isp_resource_node   *isp_res;
	struct cam_csid_hw_halt_args    halt;
	struct cam_hw_intf             *hw_intf;
	uint32_t i;
	int rc = 0;

	list_for_each_entry(hw_mgr_res, halt_list, list) {
		for (i = 0; i < CAM_ISP_HW_SPLIT_MAX; i++) {
			if (!hw_mgr_res->hw_res[i] ||
				(hw_mgr_res->hw_res[i]->res_state !=
				CAM_ISP_RESOURCE_STATE_STREAMING))
				continue;

			isp_res = hw_mgr_res->hw_res[i];
			if (isp_res->hw_intf->hw_idx != base_idx)
				continue;

			if ((isp_res->res_type == CAM_ISP_RESOURCE_PIX_PATH) &&
				(isp_res->res_id == CAM_IFE_PIX_PATH_RES_IPP)) {
				hw_intf         = isp_res->hw_intf;
				halt.node_res   = isp_res;
				halt.halt_mode  = halt_mode;
				rc = hw_intf->hw_ops.process_cmd(
					hw_intf->hw_priv,
					CAM_ISP_HW_CMD_CSID_CHANGE_HALT_MODE,
					&halt,
					sizeof(struct cam_csid_hw_halt_args));
				if (rc)
					CAM_ERR(CAM_ISP, "Halt update failed");
				break;
			}
		}
	}

	return rc;
}

static int cam_ife_mgr_csid_stop_hw(
	struct cam_ife_hw_mgr_ctx *ctx, struct list_head  *stop_list,
		uint32_t  base_idx, uint32_t stop_cmd)
{
	struct cam_ife_hw_mgr_res      *hw_mgr_res;
	struct cam_isp_resource_node   *isp_res;
	struct cam_isp_resource_node   *stop_res[CAM_IFE_PIX_PATH_RES_MAX - 1];
	struct cam_csid_hw_stop_args    stop;
	struct cam_hw_intf             *hw_intf;
	uint32_t i, cnt;

	cnt = 0;
	list_for_each_entry(hw_mgr_res, stop_list, list) {
		for (i = 0; i < CAM_ISP_HW_SPLIT_MAX; i++) {
			if (!hw_mgr_res->hw_res[i] ||
				(hw_mgr_res->hw_res[i]->res_state !=
				CAM_ISP_RESOURCE_STATE_STREAMING))
				continue;

			isp_res = hw_mgr_res->hw_res[i];
			if (isp_res->hw_intf->hw_idx != base_idx)
				continue;
			CAM_DBG(CAM_ISP, "base_idx %d res_id %d cnt %u",
				base_idx, isp_res->res_id, cnt);
			stop_res[cnt] = isp_res;
			cnt++;
		}
	}

	if (cnt) {
		hw_intf =  stop_res[0]->hw_intf;
		stop.num_res = cnt;
		stop.node_res = stop_res;
		stop.stop_cmd = stop_cmd;
		hw_intf->hw_ops.stop(hw_intf->hw_priv, &stop, sizeof(stop));
	}

	return 0;
}

static int cam_ife_mgr_stop_hw(void *hw_mgr_priv, void *stop_hw_args)
{
	int                               rc        = 0;
	struct cam_hw_stop_args          *stop_args = stop_hw_args;
	struct cam_isp_stop_args         *stop_isp;
	struct cam_ife_hw_mgr_res        *hw_mgr_res;
	struct cam_ife_hw_mgr_ctx        *ctx;
	enum cam_ife_csid_halt_cmd        csid_halt_type;
	uint32_t                          i, master_base_idx = 0;
	unsigned long                     rem_jiffies = 0;

	if (!hw_mgr_priv || !stop_hw_args) {
		CAM_ERR(CAM_ISP, "Invalid arguments");
		return -EINVAL;
	}

	ctx = (struct cam_ife_hw_mgr_ctx *)stop_args->ctxt_to_hw_map;
	if (!ctx || !ctx->ctx_in_use) {
		CAM_ERR(CAM_ISP, "Invalid context is used");
		return -EPERM;
	}

	CAM_DBG(CAM_ISP, " Enter...ctx id:%d", ctx->ctx_index);
	stop_isp = (struct cam_isp_stop_args    *)stop_args->args;

	/* Set the csid halt command */
	if ((stop_isp->hw_stop_cmd == CAM_ISP_HW_STOP_AT_FRAME_BOUNDARY) ||
		ctx->dsp_enabled)
		csid_halt_type = CAM_CSID_HALT_AT_FRAME_BOUNDARY;
	else
		csid_halt_type = CAM_CSID_HALT_IMMEDIATELY;

	/* Note:stop resource will remove the irq mask from the hardware */

	if (!ctx->num_base) {
		CAM_ERR(CAM_ISP, "number of bases are zero");
		return -EINVAL;
	}

	CAM_DBG(CAM_ISP, "Halting CSIDs");

	/* get master base index first */
	for (i = 0; i < ctx->num_base; i++) {
		if (ctx->base[i].split_id == CAM_ISP_HW_SPLIT_LEFT) {
			master_base_idx = ctx->base[i].idx;
			break;
		}
	}

	/*
	 * If Context does not have PIX resources and has only RDI resource
	 * then take the first base index.
	 */
	if (i == ctx->num_base)
		master_base_idx = ctx->base[0].idx;

	/*Change slave mode*/
	if (csid_halt_type == CAM_CSID_HALT_IMMEDIATELY) {
		for (i = 0; i < ctx->num_base; i++) {
			if (ctx->base[i].idx == master_base_idx)
				continue;
			cam_ife_mgr_csid_change_halt_mode(
				&ctx->res_list_ife_csid,
				ctx->base[i].idx,
				CAM_CSID_HALT_MODE_INTERNAL);
		}
	}

	CAM_DBG(CAM_ISP, "Stopping master CSID idx %d", master_base_idx);

	/* Stop the master CSID path first */
	cam_ife_mgr_csid_stop_hw(ctx, &ctx->res_list_ife_csid,
		master_base_idx, csid_halt_type);

	/* stop rest of the CSID paths  */
	for (i = 0; i < ctx->num_base; i++) {
		if (ctx->base[i].idx == master_base_idx)
			continue;
		CAM_DBG(CAM_ISP, "Stopping CSID idx %d i %d master %d",
			ctx->base[i].idx, i, master_base_idx);

		cam_ife_mgr_csid_stop_hw(ctx, &ctx->res_list_ife_csid,
			ctx->base[i].idx, csid_halt_type);
	}

	CAM_DBG(CAM_ISP, "Stopping master CID idx %d", master_base_idx);

	/* Stop the master CIDs first */
	cam_ife_mgr_csid_stop_hw(ctx, &ctx->res_list_ife_cid,
			master_base_idx, csid_halt_type);

	/* stop rest of the CIDs */
	for (i = 0; i < ctx->num_base; i++) {
		if (ctx->base[i].idx == master_base_idx)
			continue;
		CAM_DBG(CAM_ISP, "Stopping CID idx %d i %d master %d",
			ctx->base[i].idx, i, master_base_idx);
		cam_ife_mgr_csid_stop_hw(ctx, &ctx->res_list_ife_cid,
			ctx->base[i].idx, csid_halt_type);
	}

	CAM_DBG(CAM_ISP, "Going to stop IFE Out");

	/* IFE out resources */
	for (i = 0; i < CAM_IFE_HW_OUT_RES_MAX; i++)
		cam_ife_hw_mgr_stop_hw_res(&ctx->res_list_ife_out[i]);

	CAM_DBG(CAM_ISP, "Going to stop IFE Mux");

	/* IFE mux in resources */
	list_for_each_entry(hw_mgr_res, &ctx->res_list_ife_src, list) {
		cam_ife_hw_mgr_stop_hw_res(hw_mgr_res);
	}

	/* IFE bus rd resources */
	list_for_each_entry(hw_mgr_res, &ctx->res_list_ife_in_rd, list) {
		cam_ife_hw_mgr_stop_hw_res(hw_mgr_res);
	}

	cam_tasklet_stop(ctx->common.tasklet_info);

	cam_ife_mgr_pause_hw(ctx);

	rem_jiffies = wait_for_completion_timeout(&ctx->config_done_complete,
		msecs_to_jiffies(10));
	if (rem_jiffies == 0) {
		CAM_WARN(CAM_ISP,
			"config done completion timeout for last applied req_id=%llu rc=%d ctx_index %d",
			ctx->applied_req_id, rc, ctx->ctx_index);
		rc = -ETIMEDOUT;
	}

	if (stop_isp->stop_only)
		goto end;

	if (cam_cdm_stream_off(ctx->cdm_handle))
		CAM_ERR(CAM_ISP, "CDM stream off failed %d", ctx->cdm_handle);

	cam_ife_hw_mgr_deinit_hw(ctx);
	CAM_DBG(CAM_ISP,
		"Stop success for ctx id:%d rc :%d", ctx->ctx_index, rc);

	mutex_lock(&g_ife_hw_mgr.ctx_mutex);
	if (!atomic_dec_return(&g_ife_hw_mgr.active_ctx_cnt)) {
		rc = cam_ife_notify_safe_lut_scm(CAM_IFE_SAFE_DISABLE);
		if (rc) {
			CAM_ERR(CAM_ISP,
				"SAFE SCM call failed:Check TZ/HYP dependency");
			rc = 0;
		}
	}
	mutex_unlock(&g_ife_hw_mgr.ctx_mutex);

end:
	return rc;
}

