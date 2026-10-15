// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2020, The Linux Foundation. All rights reserved.
 */

int32_t cam_csiphy_disable_hw(struct csiphy_device *csiphy_dev)
{
	struct cam_hw_soc_info   *soc_info;

	if (!csiphy_dev || !csiphy_dev->ref_count) {
		CAM_ERR(CAM_CSIPHY, "csiphy dev NULL / ref_count ZERO");
		return 0;
	}
	soc_info = &csiphy_dev->soc_info;

	if (--csiphy_dev->ref_count) {
		CAM_ERR(CAM_CSIPHY, "csiphy refcount = %d",
			csiphy_dev->ref_count);
		return 0;
	}

	cam_csiphy_reset(csiphy_dev);

	cam_soc_util_disable_platform_resource(soc_info, true, true);

	return 0;
}
