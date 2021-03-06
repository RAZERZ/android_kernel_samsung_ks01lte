/*
 * Copyright (c) 2015 The CyanogenMod Project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include "mdss_dsi.h"
#include "mdss_fb.h"
#include "mdss_mdp.h"
#include "mdss_livedisplay.h"

/*
 * LiveDisplay is the display management service in CyanogenMod. It uses
 * various capabilities of the hardware and software in order to
 * optimize the experience for ambient conditions and time of day.
 *
 * This module is initialized by mdss_fb for each panel, and creates
 * several new controls in /sys/class/graphics/fbX based on the
 * configuration in the devicetree.
 *
 * rgb: Always available with MDSS. Used for color temperature and
 *      user-level calibration. Takes a string of "r g b".
 * 
 * Removed everything else as it is part of mDNIe or 
 * Samsung panel driver.
 */

/**
 * simple color temperature interface using polynomial color correction
 *
 * input values are r/g/b adjustments from 0-32768 representing 0 -> 1
 *
 * example adjustment @ 3500K:
 * 1.0000 / 0.5515 / 0.2520 = 32768 / 25828 / 17347
 *
 * reference chart:
 * http://www.vendian.org/mncharity/dir3/blackbody/UnstableURLs/bbr_color.html
 */
static int mdss_livedisplay_update_pcc(struct mdss_livedisplay_ctx *mlc)
{
	static struct mdp_pcc_cfg_data pcc_cfg;

	if (mlc == NULL)
		return -ENODEV;

	WARN_ON(!mutex_is_locked(&mlc->lock));

	pr_info("%s: r=%d g=%d b=%d\n", __func__, mlc->r, mlc->g, mlc->b);

	memset(&pcc_cfg, 0, sizeof(struct mdp_pcc_cfg_data));

	pcc_cfg.block = mlc->mfd->index + MDP_LOGICAL_BLOCK_DISP_0;
	if (mlc->r == 32768 && mlc->g == 32768 && mlc->b == 32768)
		pcc_cfg.ops = MDP_PP_OPS_DISABLE;
	else
		pcc_cfg.ops = MDP_PP_OPS_ENABLE;
	pcc_cfg.ops |= MDP_PP_OPS_WRITE;
	pcc_cfg.r.r = mlc->r;
	pcc_cfg.g.g = mlc->g;
	pcc_cfg.b.b = mlc->b;

	return mdss_mdp_user_pcc_config(&pcc_cfg);
}

/*
 * Update all or a subset of parameters
 */
static void mdss_livedisplay_worker(struct work_struct *work)
{
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = NULL;
	struct mdss_panel_info *pinfo = NULL;
	struct mdss_livedisplay_ctx *mlc = NULL;

	mlc = container_of(work, struct mdss_livedisplay_ctx, update_work);

	if (mlc == NULL)
		return;

	ctrl_pdata = get_ctrl(mlc->mfd);
	if (ctrl_pdata == NULL)
		return;

	pinfo = &(ctrl_pdata->panel_data.panel_info);
	if (pinfo == NULL)
		return;

	if (!mdss_panel_is_power_on_interactive(pinfo->panel_power_state))
		return;

	mutex_lock(&mlc->lock);

	// Restore saved RGB settings
	if (mlc->updated & MODE_RGB)
		mdss_livedisplay_update_pcc(mlc);

out:
	mlc->updated = 0;
	mutex_unlock(&mlc->lock);
}

void mdss_livedisplay_update(struct mdss_livedisplay_ctx *mlc, uint32_t updated)
{
	mutex_lock(&mlc->lock);
	mlc->updated |= updated;
	mutex_unlock(&mlc->lock);

	queue_work(mlc->wq, &mlc->update_work);
}

static ssize_t mdss_livedisplay_get_rgb(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_livedisplay_ctx *mlc;

	if (mfd == NULL)
		return -ENODEV;

	mlc = get_ctx(mfd);

	return scnprintf(buf, PAGE_SIZE, "%d %d %d\n",
			mlc->r, mlc->g, mlc->b);
}

static ssize_t mdss_livedisplay_set_rgb(struct device *dev,
							struct device_attribute *attr,
							const char *buf, size_t count)
{
	uint32_t r = 0, g = 0, b = 0;
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_panel_data *pdata;
	struct mdss_livedisplay_ctx *mlc;
	int ret = -EINVAL;

	if (mfd == NULL)
		return -ENODEV;

	if (count > 19)
		return -EINVAL;

	mlc = get_ctx(mfd);
	pdata = dev_get_platdata(&mfd->pdev->dev);

	sscanf(buf, "%d %d %d", &r, &g, &b);

	if (r < 0 || r > 32768)
		return -EINVAL;
	if (g < 0 || g > 32768)
		return -EINVAL;
	if (b < 0 || b > 32768)
		return -EINVAL;

	mutex_lock(&mlc->lock);
	mlc->r = r;
	mlc->g = g;
	mlc->b = b;
	mutex_unlock(&mlc->lock);

	mdss_livedisplay_update(mlc, MODE_RGB);
	ret = count;

	return ret;
}

static DEVICE_ATTR(rgb, S_IRUGO | S_IWUSR | S_IWGRP, mdss_livedisplay_get_rgb, mdss_livedisplay_set_rgb);

int mdss_livedisplay_parse_dt(struct device_node *np, struct mdss_panel_info *pinfo)
{
	struct mdss_livedisplay_ctx *mlc;

	if (pinfo == NULL)
		return -ENODEV;

	mlc = kzalloc(sizeof(struct mdss_livedisplay_ctx), GFP_KERNEL);

	mlc->r = mlc->g = mlc->b = 32768;
	mlc->updated = 0;

	mutex_init(&mlc->lock);

	mlc->wq = create_singlethread_workqueue("livedisplay_wq");
	INIT_WORK(&mlc->update_work, mdss_livedisplay_worker);

	pinfo->livedisplay = mlc;
	return 0;
}

int mdss_livedisplay_create_sysfs(struct msm_fb_data_type *mfd)
{
	int rc = 0;
	struct mdss_livedisplay_ctx *mlc = get_ctx(mfd);

	if (mlc == NULL)
		return 0;

	rc = sysfs_create_file(&mfd->fbi->dev->kobj, &dev_attr_rgb.attr);
	if (rc)
		goto sysfs_err;

	mlc->mfd = mfd;

	return rc;

sysfs_err:
	pr_err("%s: sysfs creation failed, rc=%d", __func__, rc);
	return rc;
}

