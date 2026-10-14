// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012-2020, The Linux Foundation. All rights reserved.
 */

static int dwc3_otg_start_host(struct dwc3_msm *mdwc, int on)
{
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);
	int ret = 0;

	/*
	 * The vbus_reg pointer could have multiple values
	 * NULL: regulator_get() hasn't been called, or was previously deferred
	 * IS_ERR: regulator could not be obtained, so skip using it
	 * Valid pointer otherwise
	 */
	if (!mdwc->vbus_reg) {
		mdwc->vbus_reg = devm_regulator_get_optional(mdwc->dev,
					"vbus_dwc3");
		if (IS_ERR(mdwc->vbus_reg) &&
				PTR_ERR(mdwc->vbus_reg) == -EPROBE_DEFER) {
			/* regulators may not be ready, so retry again later */
			mdwc->vbus_reg = NULL;
			return -EPROBE_DEFER;
		}
	}

	if (on) {
		dev_dbg(mdwc->dev, "%s: turn on host\n", __func__);

		mdwc->hs_phy->flags |= PHY_HOST_MODE;
		pm_runtime_get_sync(mdwc->dev);
		dbg_event(0xFF, "StrtHost gync",
			atomic_read(&mdwc->dev->power.usage_count));
		if (dwc->maximum_speed >= USB_SPEED_SUPER) {
			mdwc->ss_phy->flags |= PHY_HOST_MODE;
			usb_phy_notify_connect(mdwc->ss_phy,
						USB_SPEED_SUPER);
		}

		usb_phy_notify_connect(mdwc->hs_phy, USB_SPEED_HIGH);
		if (!IS_ERR_OR_NULL(mdwc->vbus_reg))
			ret = regulator_enable(mdwc->vbus_reg);
		if (ret) {
			dev_err(mdwc->dev, "unable to enable vbus_reg\n");
			mdwc->hs_phy->flags &= ~PHY_HOST_MODE;
			mdwc->ss_phy->flags &= ~PHY_HOST_MODE;
			pm_runtime_put_sync(mdwc->dev);
			dbg_event(0xFF, "vregerr psync",
				atomic_read(&mdwc->dev->power.usage_count));
			return ret;
		}


		mdwc->host_nb.notifier_call = dwc3_msm_host_notifier;
		usb_register_notify(&mdwc->host_nb);

		dwc3_set_prtcap(dwc, DWC3_GCTL_PRTCAP_HOST);
		dwc3_en_sleep_mode(dwc);
		mdwc->usbdev_nb.notifier_call = msm_dwc3_usbdev_notify;
		usb_register_atomic_notify(&mdwc->usbdev_nb);
		ret = dwc3_host_init(dwc);
		if (ret) {
			dev_err(mdwc->dev,
				"%s: failed to add XHCI pdev ret=%d\n",
				__func__, ret);
			if (!IS_ERR_OR_NULL(mdwc->vbus_reg))
				regulator_disable(mdwc->vbus_reg);

			mdwc->hs_phy->flags &= ~PHY_HOST_MODE;
			mdwc->ss_phy->flags &= ~PHY_HOST_MODE;
			pm_runtime_put_sync(mdwc->dev);
			dbg_event(0xFF, "pdeverr psync",
				atomic_read(&mdwc->dev->power.usage_count));
			usb_unregister_notify(&mdwc->host_nb);
			return ret;
		}

		mdwc->in_host_mode = true;
		dwc3_usb3_phy_suspend(dwc, true);

		/* Reduce the U3 exit handshake timer from 8us to approximately
		 * 300ns to avoid lfps handshake interoperability issues
		 */
		if (dwc->revision == DWC3_USB31_REVISION_170A) {
			dwc3_msm_write_reg_field(mdwc->base,
					DWC31_LINK_LU3LFPSRXTIM(0),
					GEN2_U3_EXIT_RSP_RX_CLK_MASK, 6);
			dwc3_msm_write_reg_field(mdwc->base,
					DWC31_LINK_LU3LFPSRXTIM(0),
					GEN1_U3_EXIT_RSP_RX_CLK_MASK, 5);
			dev_dbg(mdwc->dev, "LU3:%08x\n",
				dwc3_msm_read_reg(mdwc->base,
					DWC31_LINK_LU3LFPSRXTIM(0)));
		}

		/* xHCI should have incremented child count as necessary */
		dbg_event(0xFF, "StrtHost psync",
			atomic_read(&mdwc->dev->power.usage_count));
		pm_runtime_mark_last_busy(mdwc->dev);
		pm_runtime_put_sync_autosuspend(mdwc->dev);
#ifdef CONFIG_SMP
		mdwc->pm_qos_req_dma.type = PM_QOS_REQ_AFFINE_IRQ;
		mdwc->pm_qos_req_dma.irq = dwc->irq;
#endif
		pm_qos_add_request(&mdwc->pm_qos_req_dma,
				PM_QOS_CPU_DMA_LATENCY, PM_QOS_DEFAULT_VALUE);
		/* start in perf mode for better performance initially */
		msm_dwc3_perf_vote_update(mdwc, true);
		schedule_delayed_work(&mdwc->perf_vote_work,
				msecs_to_jiffies(1000 * PM_QOS_SAMPLE_SEC));
	} else {
		dev_dbg(mdwc->dev, "%s: turn off host\n", __func__);

		usb_unregister_atomic_notify(&mdwc->usbdev_nb);
		if (!IS_ERR_OR_NULL(mdwc->vbus_reg))
			ret = regulator_disable(mdwc->vbus_reg);
		if (ret) {
			dev_err(mdwc->dev, "unable to disable vbus_reg\n");
			return ret;
		}

		cancel_delayed_work_sync(&mdwc->perf_vote_work);
		msm_dwc3_perf_vote_update(mdwc, false);
		pm_qos_remove_request(&mdwc->pm_qos_req_dma);

		pm_runtime_get_sync(mdwc->dev);
		dbg_event(0xFF, "StopHost gsync",
			atomic_read(&mdwc->dev->power.usage_count));
		usb_phy_notify_disconnect(mdwc->hs_phy, USB_SPEED_HIGH);
		if (mdwc->ss_phy->flags & PHY_HOST_MODE) {
			usb_phy_notify_disconnect(mdwc->ss_phy,
					USB_SPEED_SUPER);
			mdwc->ss_phy->flags &= ~PHY_HOST_MODE;
		}

		mdwc->hs_phy->flags &= ~PHY_HOST_MODE;
		dwc3_host_exit(dwc);
		usb_unregister_notify(&mdwc->host_nb);

		dwc3_set_prtcap(dwc, DWC3_GCTL_PRTCAP_DEVICE);
		dwc3_usb3_phy_suspend(dwc, false);
		mdwc->in_host_mode = false;

		/* wait for LPM, to ensure h/w is reset after stop_host */
		set_bit(WAIT_FOR_LPM, &mdwc->inputs);

		pm_runtime_put_sync_suspend(mdwc->dev);
		dbg_event(0xFF, "StopHost psync",
			atomic_read(&mdwc->dev->power.usage_count));
	}

	return 0;
}
