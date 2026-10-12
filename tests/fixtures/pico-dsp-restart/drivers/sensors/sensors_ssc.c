// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2012-2018, The Linux Foundation. All rights reserved. */
static ssize_t slpi_ssr_store(struct kobject *kobj,
	struct kobj_attribute *attr,
	const char *buf,
	size_t count)
{
	int ssr_cmd = 0;
	struct subsys_device *sns_dev = NULL;
	struct platform_device *pdev = slpi_private;
	struct slpi_loader_private *priv = NULL;

	pr_debug("%s: going to call slpi_ssr\n", __func__);

	if (kstrtoint(buf, 10, &ssr_cmd) < 0)
		return -EINVAL;

	if (ssr_cmd != SSR_RESET_CMD)
		return -EINVAL;

	priv = platform_get_drvdata(pdev);
	if (!priv)
		return -EINVAL;

	sns_dev = (struct subsys_device *)priv->pil_h;
	if (!sns_dev)
		return -EINVAL;

	dev_err(&pdev->dev, "Something went wrong with SLPI, restarting\n");

	/* subsystem_restart_dev has worker queue to handle */
	if (subsystem_restart_dev(sns_dev) != 0) {
		dev_err(&pdev->dev, "subsystem_restart_dev failed\n");
		return -EINVAL;
	}

	dev_dbg(&pdev->dev, "SLPI restarted\n");
	return count;
}
