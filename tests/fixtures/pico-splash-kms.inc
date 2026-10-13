/*
 * Copyright (c) 2014-2021, The Linux Foundation. All rights reserved.
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define pr_fmt(fmt)	"[drm:%s:%d] " fmt, __func__, __LINE__

static int sde_kms_cont_splash_config(struct msm_kms *kms)
{
	void *display;
	struct dsi_display *dsi_display;
	struct msm_display_info info;
	struct drm_encoder *encoder = NULL;
	struct drm_crtc *crtc = NULL;
	int i, rc = 0;
	struct drm_display_mode *drm_mode = NULL;
	struct drm_device *dev;
	struct msm_drm_private *priv;
	struct sde_kms *sde_kms;
	struct drm_connector_list_iter conn_iter;
	struct drm_connector *connector = NULL;
	struct sde_connector *sde_conn = NULL;
	struct sde_splash_display *splash_display;

	if (!kms) {
		SDE_ERROR("invalid kms\n");
		return -EINVAL;
	}

	sde_kms = to_sde_kms(kms);
	dev = sde_kms->dev;
	if (!dev) {
		SDE_ERROR("invalid device\n");
		return -EINVAL;
	}

	if (!sde_kms->splash_data.num_splash_regions ||
			!sde_kms->splash_data.num_splash_displays) {
		DRM_INFO("cont_splash feature not enabled\n");
		return rc;
	}

	DRM_INFO("cont_splash enabled in %d of %d display(s)\n",
				sde_kms->splash_data.num_splash_displays,
				sde_kms->dsi_display_count);

	/* dsi */
	for (i = 0; i < sde_kms->dsi_display_count; ++i) {
		display = sde_kms->dsi_displays[i];
		dsi_display = (struct dsi_display *)display;
		splash_display = &sde_kms->splash_data.splash_display[i];

		if (!splash_display->cont_splash_enabled) {
			SDE_DEBUG("display->name = %s splash not enabled\n",
					dsi_display->name);
			continue;
		}

		SDE_DEBUG("display->name = %s\n", dsi_display->name);

		if (dsi_display->bridge->base.encoder) {
			encoder = dsi_display->bridge->base.encoder;
			SDE_DEBUG("encoder name = %s\n", encoder->name);
		}
		memset(&info, 0x0, sizeof(info));
		rc = dsi_display_get_info(NULL, &info, display);
		if (rc) {
			SDE_ERROR("dsi get_info %d failed\n", i);
			encoder = NULL;
			continue;
		}
		SDE_DEBUG("info.is_connected = %s, info.display_type = %d\n",
			((info.is_connected) ? "true" : "false"),
			info.display_type);

		if (!encoder) {
			SDE_ERROR("encoder not initialized\n");
			return -EINVAL;
		}

		priv = sde_kms->dev->dev_private;
		encoder->crtc = priv->crtcs[i];
		crtc = encoder->crtc;
		splash_display->encoder =  encoder;

		SDE_DEBUG("for dsi-display:%d crtc id = %d enc id =%d\n",
				i, crtc->base.id, encoder->base.id);

		mutex_lock(&dev->mode_config.mutex);
		drm_connector_list_iter_begin(dev, &conn_iter);
		drm_for_each_connector_iter(connector, &conn_iter) {
			/**
			 * SDE_KMS doesn't attach more than one encoder to
			 * a DSI connector. So it is safe to check only with
			 * the first encoder entry. Revisit this logic if we
			 * ever have to support continuous splash for
			 * external displays in MST configuration.
			 */
			if (connector->encoder_ids[0] == encoder->base.id)
				break;
		}
		drm_connector_list_iter_end(&conn_iter);

		if (!connector) {
			SDE_ERROR("connector not initialized\n");
			mutex_unlock(&dev->mode_config.mutex);
			return -EINVAL;
		}

		if (connector->funcs->fill_modes) {
			connector->funcs->fill_modes(connector,
					dev->mode_config.max_width,
					dev->mode_config.max_height);
		} else {
			SDE_ERROR("fill_modes api not defined\n");
			mutex_unlock(&dev->mode_config.mutex);
			return -EINVAL;
		}
		mutex_unlock(&dev->mode_config.mutex);

		crtc->state->encoder_mask = (1 << drm_encoder_index(encoder));

		/* currently consider modes[0] as the preferred mode */
		drm_mode = list_first_entry(&connector->modes,
				struct drm_display_mode, head);
		SDE_DEBUG("drm_mode->name = %s, id=%d, type=0x%x, flags=0x%x\n",
				drm_mode->name, drm_mode->base.id,
				drm_mode->type, drm_mode->flags);

		/* Update CRTC drm structure */
		crtc->state->active = true;
		rc = drm_atomic_set_mode_for_crtc(crtc->state, drm_mode);
		if (rc) {
			SDE_ERROR("Failed: set mode for crtc. rc = %d\n", rc);
			return rc;
		}
		drm_mode_copy(&crtc->state->adjusted_mode, drm_mode);
		drm_mode_copy(&crtc->mode, drm_mode);

		/* Update encoder structure */
		sde_encoder_update_caps_for_cont_splash(encoder,
				splash_display, true);

		sde_crtc_update_cont_splash_settings(crtc);

		sde_conn = to_sde_connector(connector);
		if (sde_conn && sde_conn->ops.cont_splash_config)
			sde_conn->ops.cont_splash_config(sde_conn->display);

		rc = _sde_kms_update_planes_for_cont_splash(sde_kms,
				splash_display, crtc);
		if (rc) {
			SDE_ERROR("Failed: updating plane status rc=%d\n", rc);
			return rc;
		}
	}

	return rc;
}
