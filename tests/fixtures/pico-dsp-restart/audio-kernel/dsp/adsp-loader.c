// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2012-2014, 2017-2020, The Linux Foundation. All rights reserved. */
static ssize_t adsp_ssr_store(struct kobject *kobj,
	struct kobj_attribute *attr,
	const char *buf,
	size_t count)
{
	int ssr_command = 0;
	struct subsys_device *adsp_dev = NULL;
	struct platform_device *pdev = adsp_private;
	struct adsp_loader_private *priv = NULL;
	int rc;

	dev_dbg(&pdev->dev, "%s: going to call adsp ssr\n ", __func__);

	if (kstrtoint(buf, 10, &ssr_command) < 0)
		return -EINVAL;

	if (ssr_command != SSR_RESET_CMD)
		return -EINVAL;

	priv = platform_get_drvdata(pdev);
	if (!priv)
		return -EINVAL;

	adsp_dev = (struct subsys_device *)priv->pil_h;
	if (!adsp_dev)
		return -EINVAL;

	dev_err(&pdev->dev, "requesting for ADSP restart\n");

	/* subsystem_restart_dev has worker queue to handle */
	rc = subsystem_restart_dev(adsp_dev);
	if (rc) {
		dev_err(&pdev->dev, "subsystem_restart_dev failed\n");
		return rc;
	}

	dev_dbg(&pdev->dev, "ADSP restarted\n");
	return count;
}
