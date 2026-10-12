// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2016-2020, The Linux Foundation. All rights reserved. */
int cnss_wlan_rffemid;

static int cnss_select_pinctrl_enable(struct cnss_plat_data *plat_priv)
{
	int ret = 0, bt_en_gpio = plat_priv->pinctrl_info.bt_en_gpio;
	u8 wlan_en_state = 0;

	cnss_pr_dbg("Enter function %s:%d \n",__func__,__LINE__);
	if (bt_en_gpio < 0 || plat_priv->device_id != QCA6490_DEVICE_ID ||
	    plat_priv->device_id != QCA6390_DEVICE_ID)
		goto set_wlan_en;

	cnss_pr_dbg("Enter function %s:%d \n",__func__,__LINE__);
	if (gpio_get_value(bt_en_gpio)) {
		cnss_pr_dbg("BT_EN_GPIO State: On\n");
		ret = cnss_select_pinctrl_state(plat_priv, true);
		if (!ret)
			return ret;
		wlan_en_state = 1;
	}
	cnss_pr_dbg("Enter function %s:%d \n",__func__,__LINE__);
	if (!gpio_get_value(bt_en_gpio)) {
		cnss_pr_dbg("BT_EN_GPIO State: Off. Delay WLAN_GPIO enable\n");
		/* check for BT_EN_GPIO down race during above operation */
		if (wlan_en_state) {
			cnss_pr_dbg("Reset WLAN_EN as BT got turned off during enable\n");
			cnss_select_pinctrl_state(plat_priv, false);
			wlan_en_state = 0;
		}
		/* 100 ms delay for BT_EN and WLAN_EN QCA6490 PMU sequencing */
		msleep(100);
	}
	cnss_pr_dbg("Enter function %s:%d \n",__func__,__LINE__);

set_wlan_en:

	cnss_pr_dbg("Enter function %s:%d \n",__func__,__LINE__);
	gpio_direction_output(plat_priv->pinctrl_info.wlan_rffemid_gpio, 0);
	gpio_direction_input(plat_priv->pinctrl_info.wlan_rffemid_gpio);
	msleep(10);
	if(gpio_get_value(plat_priv->pinctrl_info.wlan_rffemid_gpio)) {
		cnss_wlan_rffemid = 1;
		cnss_pr_dbg("RF FEMID is : 1\n");
	} else {
		cnss_wlan_rffemid = 0;
		cnss_pr_dbg("RF FEMID is : 0\n");
	}
	cnss_pr_err("Enter %s: %d\n cnss_wlan_rffemid=%d ",
		     __func__,__LINE__,cnss_wlan_rffemid);

	if (!wlan_en_state)
		ret = cnss_select_pinctrl_state(plat_priv, true);
	return ret;
}

int cnss_power_on_device(struct cnss_plat_data *plat_priv)
{
	int ret = 0;

	if (plat_priv->powered_on) {
		cnss_pr_dbg("Already powered up");
		return 0;
	}

	ret = cnss_vreg_on_type(plat_priv, CNSS_VREG_PRIM);
	if (ret) {
		cnss_pr_err("Failed to turn on vreg, err = %d\n", ret);
		goto out;
	}

	ret = cnss_clk_on(plat_priv, &plat_priv->clk_list);
	if (ret) {
		cnss_pr_err("Failed to turn on clocks, err = %d\n", ret);
		goto vreg_off;
	}

	ret = cnss_select_pinctrl_enable(plat_priv);
	if (ret) {
		cnss_pr_err("Failed to select pinctrl state, err = %d\n", ret);
		goto clk_off;
	}

	plat_priv->powered_on = true;

	return 0;

clk_off:
	cnss_clk_off(plat_priv, &plat_priv->clk_list);
vreg_off:
	cnss_vreg_off_type(plat_priv, CNSS_VREG_PRIM);
out:
	return ret;
}
