// SPDX-License-Identifier: GPL-2.0+

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_encoder.h>
#include <drm/drm_managed.h>
#include <drm/drm_probe_helper.h>

#include "castkms_config.h"
#include "castkms_connector.h"

static enum drm_connector_status castkms_connector_detect(struct drm_connector *connector,
						       bool force)
{
	struct drm_device *dev = connector->dev;
	struct castkms_device *castkmsdev = drm_device_to_castkms_device(dev);
	struct castkms_connector *castkms_connector;
	enum drm_connector_status status;
	struct castkms_config_connector *connector_cfg;
	int idx;

	castkms_connector = drm_connector_to_castkms_connector(connector);

	/*
	 * Configfs owns the configuration and can release it after unplug. Keep
	 * the last status when teardown has blocked access to that configuration.
	 */
	status = connector->status;

	if (!drm_dev_enter(dev, &idx))
		return status;

	if (!castkmsdev->config)
		goto out;

	castkms_config_for_each_connector(castkmsdev->config, connector_cfg) {
		if (connector_cfg->connector == castkms_connector)
			status = castkms_config_connector_get_status(connector_cfg);
	}

out:
	drm_dev_exit(idx);

	return status;
}

static const struct drm_connector_funcs castkms_connector_funcs = {
	.detect = castkms_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static int castkms_conn_get_modes(struct drm_connector *connector)
{
	int count;

	count = drm_edid_connector_add_modes(connector);
	if (count)
		return count;

	count = drm_add_modes_noedid(connector, XRES_MAX, YRES_MAX);
	drm_set_preferred_mode(connector, XRES_DEF, YRES_DEF);

	return count;
}

static struct drm_encoder *castkms_conn_best_encoder(struct drm_connector *connector)
{
	struct drm_encoder *encoder;

	drm_connector_for_each_possible_encoder(connector, encoder)
		return encoder;

	return NULL;
}

static const struct drm_connector_helper_funcs castkms_conn_helper_funcs = {
	.get_modes    = castkms_conn_get_modes,
	.best_encoder = castkms_conn_best_encoder,
};

struct castkms_connector *castkms_connector_init(struct castkms_device *castkmsdev)
{
	struct drm_device *dev = &castkmsdev->drm;
	struct castkms_connector *connector;
	int ret;

	connector = drmm_kzalloc(dev, sizeof(*connector), GFP_KERNEL);
	if (!connector)
		return ERR_PTR(-ENOMEM);

	ret = drmm_connector_init(dev, &connector->base, &castkms_connector_funcs,
				  DRM_MODE_CONNECTOR_VIRTUAL, NULL);
	if (ret)
		return ERR_PTR(ret);

	drm_connector_helper_add(&connector->base, &castkms_conn_helper_funcs);

	return connector;
}

void castkms_trigger_connector_hotplug(struct castkms_device *castkmsdev)
{
	struct drm_device *dev = &castkmsdev->drm;

	drm_kms_helper_hotplug_event(dev);
}

static struct drm_connector *
castkms_connector_for_crtc(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct drm_connector_list_iter conn_iter;
	struct drm_connector *connector;
	struct drm_connector *found = NULL;

	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter) {
		struct drm_encoder *encoder;
		bool matches = false;

		if (connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK)
			continue;

		drm_connector_for_each_possible_encoder(connector, encoder) {
			if (encoder->possible_crtcs & drm_crtc_mask(crtc)) {
				matches = true;
				break;
			}
		}
		if (!matches || found)
			continue;

		drm_connector_get(connector);
		found = connector;
	}
	drm_connector_list_iter_end(&conn_iter);

	return found;
}

int castkms_connector_update_edid(struct drm_crtc *crtc,
				  const struct drm_edid *drm_edid)
{
	struct drm_device *dev = crtc->dev;
	struct drm_connector *connector;
	int idx;
	int ret;

	connector = castkms_connector_for_crtc(crtc);
	if (!connector)
		return -ENOENT;

	if (!drm_dev_enter(dev, &idx)) {
		drm_connector_put(connector);
		return -ENODEV;
	}

	mutex_lock(&dev->mode_config.mutex);
	ret = drm_edid_connector_update(connector, drm_edid);
	mutex_unlock(&dev->mode_config.mutex);
	if (!ret)
		drm_kms_helper_hotplug_event(dev);
	drm_dev_exit(idx);
	drm_connector_put(connector);

	return ret;
}
