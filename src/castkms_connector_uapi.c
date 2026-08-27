// SPDX-License-Identifier: GPL-2.0-only

#include <linux/build_bug.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <drm/castkms_drm.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_file.h>

#include "castkms_capture_authority.h"
#include "castkms_connector.h"
#include "castkms_connector_uapi.h"
#include "castkms_drv.h"
#include "castkms_grant.h"
#include "castkms_limits.h"

static_assert(sizeof(struct drm_castkms_capture_set_output_edid) == 24);
static_assert(sizeof(struct drm_castkms_capture_attach_monitor) == 24);
static_assert(sizeof(struct drm_castkms_capture_detach_monitor) == 16);
static_assert(CASTKMS_MAX_EDID_SIZE == DRM_CASTKMS_CAPTURE_MAX_EDID_SIZE);

static int castkms_connector_uapi_edid_from_user(
	u32 edid_size, u64 edid_ptr, const struct drm_edid **drm_edid)
{
	const struct drm_edid *parsed;
	void *edid;

	*drm_edid = NULL;
	if (!edid_size) {
		if (edid_ptr)
			return -EINVAL;
		return 0;
	}
	if (!edid_ptr || edid_size % EDID_LENGTH ||
	    edid_size > CASTKMS_MAX_EDID_SIZE)
		return -EINVAL;

	edid = memdup_user(u64_to_user_ptr(edid_ptr), edid_size);
	if (IS_ERR(edid))
		return PTR_ERR(edid);

	parsed = drm_edid_alloc(edid, edid_size);
	kfree(edid);
	if (!parsed)
		return -ENOMEM;
	if (!drm_edid_valid(parsed)) {
		drm_edid_free(parsed);
		return -EINVAL;
	}

	*drm_edid = parsed;
	return 0;
}

int castkms_capture_set_output_edid_ioctl(struct drm_device *dev, void *data,
					  struct drm_file *file_priv)
{
	struct drm_castkms_capture_set_output_edid *args = data;
	struct castkms_device *castkmsdev = drm_device_to_castkms_device(dev);
	struct castkms_capture_authority *authority;
	struct drm_connector *connector;
	const struct drm_edid *drm_edid = NULL;
	int ret;

	if (args->flags || args->reserved)
		return -EINVAL;

	ret = castkms_connector_uapi_edid_from_user(
		args->edid_size, args->edid_ptr, &drm_edid);
	if (ret)
		return ret;

	connector = drm_connector_lookup(dev, file_priv, args->connector_id);
	if (!connector) {
		ret = -ENOENT;
		goto out_free_edid;
	}

	mutex_lock(&castkmsdev->attach_transition_lock);
	ret = castkms_grant_begin(file_priv, connector,
				  CASTKMS_CAPTURE_AUTHORITY_UPDATE_EDID,
				  &authority);
	if (ret)
		goto out_unlock_transition;

	ret = castkms_connector_update_authority_edid(connector, authority,
						      drm_edid);
	castkms_grant_end(authority);
out_unlock_transition:
	mutex_unlock(&castkmsdev->attach_transition_lock);
	drm_connector_put(connector);
out_free_edid:
	if (drm_edid)
		drm_edid_free(drm_edid);

	return ret;
}

int castkms_capture_attach_monitor_ioctl(struct drm_device *dev, void *data,
					 struct drm_file *file_priv)
{
	struct drm_castkms_capture_attach_monitor *args = data;
	struct castkms_device *castkmsdev = drm_device_to_castkms_device(dev);
	struct castkms_capture_authority *authority;
	struct drm_connector *connector;
	const struct drm_edid *drm_edid = NULL;
	u32 rights = CASTKMS_CAPTURE_AUTHORITY_MANAGE_ATTACHMENT;
	int ret;

	if (args->flags || args->reserved)
		return -EINVAL;

	ret = castkms_connector_uapi_edid_from_user(
		args->edid_size, args->edid_ptr, &drm_edid);
	if (ret)
		return ret;

	connector = drm_connector_lookup(dev, file_priv, args->connector_id);
	if (!connector) {
		ret = -ENOENT;
		goto out_free_edid;
	}

	if (drm_edid)
		rights |= CASTKMS_CAPTURE_AUTHORITY_UPDATE_EDID;
	mutex_lock(&castkmsdev->attach_transition_lock);
	ret = castkms_grant_begin(file_priv, connector, rights, &authority);
	if (!ret) {
		ret = castkms_connector_attach_monitor(connector, authority,
						       drm_edid);
		castkms_grant_end(authority);
	}
	mutex_unlock(&castkmsdev->attach_transition_lock);
	drm_connector_put(connector);

out_free_edid:
	if (drm_edid)
		drm_edid_free(drm_edid);

	return ret;
}

int castkms_capture_detach_monitor_ioctl(struct drm_device *dev, void *data,
					 struct drm_file *file_priv)
{
	struct drm_castkms_capture_detach_monitor *args = data;
	struct castkms_device *castkmsdev = drm_device_to_castkms_device(dev);
	struct castkms_capture_authority *authority;
	struct drm_connector *connector;
	bool detached = false;
	int ret;

	if (args->flags || args->reserved)
		return -EINVAL;

	connector = drm_connector_lookup(dev, file_priv, args->connector_id);
	if (!connector)
		return -ENOENT;

	mutex_lock(&castkmsdev->attach_transition_lock);
	ret = castkms_grant_begin(file_priv, connector,
				  CASTKMS_CAPTURE_AUTHORITY_MANAGE_ATTACHMENT,
				  &authority);
	if (!ret) {
		ret = castkms_connector_require_authority_attached(connector,
							    authority);
		if (!ret) {
			ret = castkms_connector_detach_monitor(connector,
							 authority);
			detached = true;
		}
		castkms_grant_end(authority);
	}
	mutex_unlock(&castkmsdev->attach_transition_lock);
	if (detached)
		castkms_capture_authority_cleanup_connector_resources(
			connector, NULL, -ENOTCONN);
	drm_connector_put(connector);

	return ret;
}
