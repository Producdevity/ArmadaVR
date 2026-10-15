// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2021, The Linux Foundation. All rights reserved.
 */

static int __cam_isp_ctx_stop_dev_in_activated_unlock(
	struct cam_context *ctx, struct cam_start_stop_dev_cmd *stop_cmd)
{
	int rc = 0;
	uint32_t i;
	struct cam_hw_stop_args          stop;
	struct cam_ctx_request          *req;
	struct cam_isp_ctx_req          *req_isp;
	struct cam_isp_context          *ctx_isp =
		(struct cam_isp_context *) ctx->ctx_priv;
	struct cam_isp_stop_args         stop_isp;

	/* stop hw first */
	if (ctx_isp->hw_ctx) {
		stop.ctxt_to_hw_map = ctx_isp->hw_ctx;

		if (stop_cmd)
			stop_isp.hw_stop_cmd =
				CAM_ISP_HW_STOP_AT_FRAME_BOUNDARY;
		else
			stop_isp.hw_stop_cmd = CAM_ISP_HW_STOP_IMMEDIATELY;

		stop_isp.stop_only = false;
		stop.args = (void *) &stop_isp;
		ctx->hw_mgr_intf->hw_stop(ctx->hw_mgr_intf->hw_mgr_priv,
			&stop);
	}

	/* Mask off all the incoming hardware events */
	spin_lock_bh(&ctx->lock);
	ctx_isp->substate_activated = CAM_ISP_CTX_ACTIVATED_HALT;
	spin_unlock_bh(&ctx->lock);
	CAM_DBG(CAM_ISP, "next Substate[%s]",
		__cam_isp_ctx_substate_val_to_type(
		ctx_isp->substate_activated));

	if (ctx->ctx_crm_intf &&
		ctx->ctx_crm_intf->notify_stop) {
		struct cam_req_mgr_notify_stop notify;

		notify.link_hdl = ctx->link_hdl;
		CAM_DBG(CAM_ISP,
			"Notify CRM about device stop ctx %u link 0x%x",
			ctx->ctx_id, ctx->link_hdl);
		ctx->ctx_crm_intf->notify_stop(&notify);
	} else
		CAM_ERR(CAM_ISP, "cb not present");

	while (!list_empty(&ctx->pending_req_list)) {
		req = list_first_entry(&ctx->pending_req_list,
				struct cam_ctx_request, list);
		list_del_init(&req->list);
		req_isp = (struct cam_isp_ctx_req *) req->req_priv;
		CAM_DBG(CAM_ISP, "signal fence in pending list. fence num %d",
			 req_isp->num_fence_map_out);
		for (i = 0; i < req_isp->num_fence_map_out; i++)
			if (req_isp->fence_map_out[i].sync_id != -1) {
				cam_sync_signal(
					req_isp->fence_map_out[i].sync_id,
					CAM_SYNC_STATE_SIGNALED_ERROR);
			}
		list_add_tail(&req->list, &ctx->free_req_list);
	}

	while (!list_empty(&ctx->wait_req_list)) {
		req = list_first_entry(&ctx->wait_req_list,
				struct cam_ctx_request, list);
		list_del_init(&req->list);
		req_isp = (struct cam_isp_ctx_req *) req->req_priv;
		CAM_DBG(CAM_ISP, "signal fence in wait list. fence num %d",
			 req_isp->num_fence_map_out);
		for (i = 0; i < req_isp->num_fence_map_out; i++)
			if (req_isp->fence_map_out[i].sync_id != -1) {
				cam_sync_signal(
					req_isp->fence_map_out[i].sync_id,
					CAM_SYNC_STATE_SIGNALED_ERROR);
			}
		list_add_tail(&req->list, &ctx->free_req_list);
	}

	while (!list_empty(&ctx->active_req_list)) {
		req = list_first_entry(&ctx->active_req_list,
				struct cam_ctx_request, list);
		list_del_init(&req->list);
		req_isp = (struct cam_isp_ctx_req *) req->req_priv;
		CAM_DBG(CAM_ISP, "signal fence in active list. fence num %d",
			 req_isp->num_fence_map_out);
		for (i = 0; i < req_isp->num_fence_map_out; i++)
			if (req_isp->fence_map_out[i].sync_id != -1) {
				cam_sync_signal(
					req_isp->fence_map_out[i].sync_id,
					CAM_SYNC_STATE_SIGNALED_ERROR);
			}
		list_add_tail(&req->list, &ctx->free_req_list);
	}

	ctx_isp->frame_id = 0;
	ctx_isp->active_req_cnt = 0;
	ctx_isp->reported_req_id = 0;
	ctx_isp->bubble_frame_cnt = 0;
	ctx_isp->last_applied_req_id = 0;
	ctx_isp->req_info.last_bufdone_req_id = 0;
	atomic_set(&ctx_isp->process_bubble, 0);
	atomic_set(&ctx_isp->rxd_epoch, 0);
	atomic64_set(&ctx_isp->state_monitor_head, -1);

	for (i = 0; i < CAM_ISP_CTX_EVENT_MAX; i++)
		atomic64_set(&ctx_isp->event_record_head[i], -1);

	CAM_DBG(CAM_ISP, "Stop device success next state %d on ctx %u",
		ctx->state, ctx->ctx_id);

	if (!stop_cmd) {
		rc = __cam_isp_ctx_unlink_in_ready(ctx, NULL);
		if (rc)
			CAM_ERR(CAM_ISP, "Unlink failed rc=%d", rc);
	}
	return rc;
}

static int __cam_isp_ctx_stop_dev_in_activated(struct cam_context *ctx,
	struct cam_start_stop_dev_cmd *cmd)
{
	int rc = 0;
	struct cam_isp_context *ctx_isp =
		(struct cam_isp_context *)ctx->ctx_priv;

	__cam_isp_ctx_stop_dev_in_activated_unlock(ctx, cmd);
	ctx_isp->init_received = false;
	ctx->state = CAM_CTX_ACQUIRED;
	trace_cam_context_state("ISP", ctx);
	return rc;
}

