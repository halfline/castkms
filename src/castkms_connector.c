// SPDX-License-Identifier: GPL-2.0+

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_encoder.h>
#include <drm/drm_file.h>
#include <drm/drm_managed.h>
#include <drm/drm_modes.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_property.h>
#include <drm/drm_sysfs.h>

#include "castkms_audio.h"
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

	if (castkms_connector->monitor_attached) {
		status = connector_status_connected;
		goto out;
	}

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
	if (!count) {
		count = drm_add_modes_noedid(connector, XRES_MAX, YRES_MAX);
		drm_set_preferred_mode(connector, XRES_DEF, YRES_DEF);
	}

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

struct castkms_connector *castkms_connector_init(struct castkms_device *castkmsdev,
						 unsigned int output_index)
{
	struct drm_device *dev = &castkmsdev->drm;
	struct castkms_connector *castkms_connector;
	struct drm_connector *connector;
	int ret;

	castkms_connector = drmm_kzalloc(dev, sizeof(*castkms_connector),
					 GFP_KERNEL);
	if (!castkms_connector)
		return ERR_PTR(-ENOMEM);

	castkms_connector->output_index = output_index;
	connector = &castkms_connector->base;
	ret = drmm_connector_init(dev, connector, &castkms_connector_funcs,
				  DRM_MODE_CONNECTOR_VIRTUAL, NULL);
	if (ret)
		return ERR_PTR(ret);

	drm_connector_helper_add(connector, &castkms_conn_helper_funcs);
	drm_connector_attach_edid_property(connector);

	if (!castkmsdev->capture_active_prop) {
		castkmsdev->capture_active_prop =
			drm_property_create_range(dev, DRM_MODE_PROP_IMMUTABLE,
						  "capture_active", 0, 1);
		if (!castkmsdev->capture_active_prop)
			return ERR_PTR(-ENOMEM);
	}
	drm_object_attach_property(&connector->base,
				   castkmsdev->capture_active_prop, 0);

	return castkms_connector;
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

static void
castkms_connector_sync_config_status(struct drm_connector *connector,
				     enum drm_connector_status status)
{
	struct drm_device *dev = connector->dev;
	struct castkms_device *castkmsdev = drm_device_to_castkms_device(dev);
	struct castkms_connector *castkms_connector =
		drm_connector_to_castkms_connector(connector);
	struct castkms_config_connector *connector_cfg;
	int idx;

	if (!drm_dev_enter(dev, &idx))
		return;

	if (castkmsdev->config) {
		castkms_config_for_each_connector(castkmsdev->config,
						  connector_cfg) {
			if (connector_cfg->connector == castkms_connector)
				castkms_config_connector_set_status(connector_cfg,
								    status);
		}
	}

	drm_dev_exit(idx);
}

static int
castkms_connector_publish_edid(struct drm_connector *connector,
			       const struct drm_edid *drm_edid)
{
	struct drm_device *dev = connector->dev;
	int idx;
	int ret;

	if (!drm_dev_enter(dev, &idx))
		return -ENODEV;

	mutex_lock(&dev->mode_config.mutex);
	ret = drm_edid_connector_update(connector, drm_edid);
	mutex_unlock(&dev->mode_config.mutex);
	if (!ret)
		drm_kms_helper_hotplug_event(dev);
	drm_dev_exit(idx);

	return ret;
}

static void
castkms_connector_set_status(struct drm_connector *connector,
			     enum drm_connector_status status)
{
	struct drm_device *dev = connector->dev;

	mutex_lock(&dev->mode_config.mutex);
	connector->status = status;
	mutex_unlock(&dev->mode_config.mutex);
}

int castkms_connector_update_edid(struct drm_crtc *crtc,
				  const struct drm_edid *drm_edid)
{
	struct drm_device *dev = crtc->dev;
	struct castkms_device *castkmsdev = drm_device_to_castkms_device(dev);
	struct drm_connector *connector;
	int ret;

	connector = castkms_connector_for_crtc(crtc);
	if (!connector)
		return -ENOENT;

	ret = castkms_connector_publish_edid(connector, drm_edid);
	if (!ret)
		castkms_audio_notify_eld(castkmsdev, connector);
	drm_connector_put(connector);

	return ret;
}

int castkms_connector_attach_monitor(struct drm_connector *connector,
				     struct drm_file *file,
				     const struct drm_edid *drm_edid)
{
	struct drm_device *dev = connector->dev;
	struct castkms_device *castkmsdev = drm_device_to_castkms_device(dev);
	struct castkms_connector *castkms_connector =
		drm_connector_to_castkms_connector(connector);
	int ret;

	if (connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK)
		return -ENOENT;

	mutex_lock(&castkmsdev->attach_lock);
	if (castkms_connector->monitor_attached) {
		mutex_unlock(&castkmsdev->attach_lock);
		return -EBUSY;
	}

	castkms_connector->monitor_attached = true;
	castkms_connector->attach_file = file;
	mutex_unlock(&castkmsdev->attach_lock);

	castkms_connector_set_status(connector, connector_status_connected);
	castkms_connector_sync_config_status(connector,
					     connector_status_connected);
	ret = castkms_connector_publish_edid(connector, drm_edid);
	if (ret) {
		mutex_lock(&castkmsdev->attach_lock);
		castkms_connector->monitor_attached = false;
		castkms_connector->attach_file = NULL;
		mutex_unlock(&castkmsdev->attach_lock);
		castkms_connector_set_status(connector,
					     connector_status_disconnected);
		castkms_connector_sync_config_status(connector,
						     connector_status_disconnected);
		return ret;
	}

	castkms_audio_notify_eld(castkmsdev, connector);

	return 0;
}

int castkms_connector_detach_monitor(struct drm_connector *connector,
				     struct drm_file *file)
{
	struct drm_device *dev = connector->dev;
	struct castkms_device *castkmsdev = drm_device_to_castkms_device(dev);
	struct castkms_connector *castkms_connector =
		drm_connector_to_castkms_connector(connector);
	int ret;

	if (connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK)
		return -ENOENT;

	mutex_lock(&castkmsdev->attach_lock);
	if (!castkms_connector->monitor_attached) {
		mutex_unlock(&castkmsdev->attach_lock);
		return -ENOTCONN;
	}
	if (castkms_connector->attach_file != file) {
		mutex_unlock(&castkmsdev->attach_lock);
		return -EACCES;
	}

	castkms_connector->monitor_attached = false;
	castkms_connector->attach_file = NULL;
	mutex_unlock(&castkmsdev->attach_lock);

	castkms_audio_notify_disconnect(castkmsdev, connector);

	castkms_connector_set_status(connector,
				     connector_status_disconnected);
	castkms_connector_sync_config_status(connector,
					     connector_status_disconnected);
	ret = castkms_connector_publish_edid(connector, NULL);

	return ret;
}

int castkms_connector_require_attached(struct drm_crtc *crtc,
				       struct drm_file *file)
{
	struct drm_device *dev = crtc->dev;
	struct castkms_device *castkmsdev = drm_device_to_castkms_device(dev);
	struct drm_connector *connector;
	struct castkms_connector *castkms_connector;
	int ret = 0;

	connector = castkms_connector_for_crtc(crtc);
	if (!connector)
		return -ENOENT;

	castkms_connector = drm_connector_to_castkms_connector(connector);
	mutex_lock(&castkmsdev->attach_lock);
	if (!castkms_connector->monitor_attached)
		ret = -ENOTCONN;
	else if (castkms_connector->attach_file != file)
		ret = -EACCES;
	mutex_unlock(&castkmsdev->attach_lock);
	drm_connector_put(connector);

	return ret;
}

void castkms_connector_set_capture_active(struct drm_crtc *crtc, bool active)
{
	struct drm_device *dev = crtc->dev;
	struct castkms_device *castkmsdev = drm_device_to_castkms_device(dev);
	struct drm_connector *connector;

	connector = castkms_connector_for_crtc(crtc);
	if (!connector)
		return;

	drm_object_property_set_value(&connector->base,
				      castkmsdev->capture_active_prop,
				      active ? 1 : 0);
	drm_sysfs_connector_property_event(connector,
					   castkmsdev->capture_active_prop);
	drm_connector_put(connector);
}

void castkms_connectors_detach_file(struct drm_device *dev,
				    struct drm_file *file)
{
	struct drm_connector *owned[CASTKMS_MAX_OUTPUT_OBJECTS];
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;
	unsigned int n = 0;
	unsigned int i;
	int idx;

	if (!drm_dev_enter(dev, &idx))
		return;

	drm_connector_list_iter_begin(dev, &iter);
	drm_for_each_connector_iter(connector, &iter) {
		struct castkms_connector *castkms_connector;

		if (connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK)
			continue;
		if (n == ARRAY_SIZE(owned))
			break;

		castkms_connector = drm_connector_to_castkms_connector(connector);
		if (castkms_connector->attach_file != file)
			continue;

		drm_connector_get(connector);
		owned[n++] = connector;
	}
	drm_connector_list_iter_end(&iter);

	for (i = 0; i < n; i++) {
		castkms_connector_detach_monitor(owned[i], file);
		drm_connector_put(owned[i]);
	}

	drm_dev_exit(idx);
}
