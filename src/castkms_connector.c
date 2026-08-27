// SPDX-License-Identifier: GPL-2.0+

#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_managed.h>
#include <drm/drm_probe_helper.h>

#include <kunit/visibility.h>

#include "castkms_capture_authority.h"
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

	if (READ_ONCE(castkms_connector->monitor_attached)) {
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

struct castkms_connector *castkms_connector_init(struct castkms_device *castkmsdev)
{
	struct drm_device *dev = &castkmsdev->drm;
	struct castkms_connector *castkms_connector;
	struct drm_connector *connector;
	int ret;

	castkms_connector = drmm_kzalloc(dev, sizeof(*castkms_connector),
					 GFP_KERNEL);
	if (!castkms_connector)
		return ERR_PTR(-ENOMEM);

	connector = &castkms_connector->base;
	ret = drmm_connector_init(dev, connector, &castkms_connector_funcs,
				  DRM_MODE_CONNECTOR_VIRTUAL, NULL);
	if (ret)
		return ERR_PTR(ret);

	drm_connector_helper_add(connector, &castkms_conn_helper_funcs);
	drm_connector_attach_edid_property(connector);

	return castkms_connector;
}

void castkms_trigger_connector_hotplug(struct castkms_device *castkmsdev)
{
	struct drm_device *dev = &castkmsdev->drm;

	drm_kms_helper_hotplug_event(dev);
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

int castkms_connector_attach_monitor(
	struct drm_connector *connector,
	struct castkms_capture_authority *authority,
	const struct drm_edid *drm_edid)
{
	struct drm_device *dev = connector->dev;
	struct castkms_device *castkmsdev = drm_device_to_castkms_device(dev);
	struct castkms_connector *castkms_connector =
		drm_connector_to_castkms_connector(connector);
	int ret;

	if (!authority)
		return -EACCES;
	if (connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK)
		return -ENOENT;

	lockdep_assert_held(&castkmsdev->attach_transition_lock);
	mutex_lock(&castkmsdev->attach_lock);
	if (castkms_connector->monitor_attached) {
		mutex_unlock(&castkmsdev->attach_lock);
		return -EBUSY;
	}

	WRITE_ONCE(castkms_connector->monitor_attached, true);
	castkms_connector->attachment_authority = authority;
	castkms_capture_authority_get(authority);
	mutex_unlock(&castkmsdev->attach_lock);

	castkms_connector_set_status(connector, connector_status_connected);
	castkms_connector_sync_config_status(connector,
					     connector_status_connected);
	ret = castkms_connector_publish_edid(connector, drm_edid);
	if (ret) {
		mutex_lock(&castkmsdev->attach_lock);
		WRITE_ONCE(castkms_connector->monitor_attached, false);
		castkms_connector->attachment_authority = NULL;
		mutex_unlock(&castkmsdev->attach_lock);
		castkms_capture_authority_put(authority);
		castkms_connector_set_status(connector,
					     connector_status_disconnected);
		castkms_connector_sync_config_status(
			connector, connector_status_disconnected);
	}

	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_connector_attach_monitor);

int castkms_connector_detach_monitor(
	struct drm_connector *connector,
	struct castkms_capture_authority *authority)
{
	struct drm_device *dev = connector->dev;
	struct castkms_device *castkmsdev = drm_device_to_castkms_device(dev);
	struct castkms_connector *castkms_connector =
		drm_connector_to_castkms_connector(connector);
	int ret;

	if (!authority)
		return -EACCES;
	if (connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK)
		return -ENOENT;

	lockdep_assert_held(&castkmsdev->attach_transition_lock);
	mutex_lock(&castkmsdev->attach_lock);
	if (!castkms_connector->monitor_attached) {
		mutex_unlock(&castkmsdev->attach_lock);
		return -ENOTCONN;
	}
	if (castkms_connector->attachment_authority != authority) {
		mutex_unlock(&castkmsdev->attach_lock);
		return -EACCES;
	}

	WRITE_ONCE(castkms_connector->monitor_attached, false);
	castkms_connector->attachment_authority = NULL;
	mutex_unlock(&castkmsdev->attach_lock);

	castkms_connector_set_status(connector,
				     connector_status_disconnected);
	castkms_connector_sync_config_status(connector,
					     connector_status_disconnected);
	ret = castkms_connector_publish_edid(connector, NULL);
	castkms_capture_authority_put(authority);

	return ret;
}

bool castkms_connector_authority_is_attached(
	struct drm_connector *connector,
	struct castkms_capture_authority *authority)
{
	struct drm_device *dev = connector->dev;
	struct castkms_device *castkmsdev = drm_device_to_castkms_device(dev);
	struct castkms_connector *castkms_connector =
		drm_connector_to_castkms_connector(connector);
	bool attached;

	mutex_lock(&castkmsdev->attach_lock);
	attached = castkms_connector->monitor_attached &&
		   castkms_connector->attachment_authority == authority;
	mutex_unlock(&castkmsdev->attach_lock);

	return attached;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_connector_authority_is_attached);

int castkms_connector_require_authority_attached(
	struct drm_connector *connector,
	struct castkms_capture_authority *authority)
{
	struct drm_device *dev = connector->dev;
	struct castkms_device *castkmsdev = drm_device_to_castkms_device(dev);
	struct castkms_connector *castkms_connector =
		drm_connector_to_castkms_connector(connector);
	int ret = 0;

	mutex_lock(&castkmsdev->attach_lock);
	if (!castkms_connector->monitor_attached)
		ret = -ENOTCONN;
	else if (castkms_connector->attachment_authority != authority)
		ret = -EACCES;
	mutex_unlock(&castkmsdev->attach_lock);

	return ret;
}

bool castkms_connector_detach_authority(
	struct castkms_capture_authority *authority)
{
	struct drm_connector *connector =
		castkms_capture_authority_connector(authority);
	struct castkms_device *castkmsdev =
		drm_device_to_castkms_device(connector->dev);
	bool detached = false;

	/* A revoked non-owner cannot become the attachment owner afterward. */
	if (!castkms_connector_authority_is_attached(connector, authority))
		return false;

	mutex_lock(&castkmsdev->attach_transition_lock);
	if (castkms_connector_authority_is_attached(connector, authority)) {
		castkms_connector_detach_monitor(connector, authority);
		detached = true;
	}
	mutex_unlock(&castkmsdev->attach_transition_lock);

	return detached;
}
