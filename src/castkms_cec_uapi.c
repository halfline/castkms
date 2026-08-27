// SPDX-License-Identifier: GPL-2.0+

#include <linux/build_bug.h>

#include <drm/castkms_drm.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>

#include "castkms_cec_core.h"
#include "castkms_cec_uapi.h"
#include "castkms_connector.h"

static_assert(sizeof(struct drm_castkms_cec_query_caps) == 40);

int castkms_cec_query_caps_ioctl(struct drm_device *dev, void *data,
				 struct drm_file *file_priv)
{
	struct drm_castkms_cec_query_caps *args = data;
	struct castkms_connector *connector;
	struct drm_connector *base;
	int idx;

	if (args->flags || args->reserved)
		return -EINVAL;
	if (!drm_dev_enter(dev, &idx))
		return -ENODEV;

	base = drm_connector_lookup(dev, file_priv, args->connector_id);
	if (!base) {
		drm_dev_exit(idx);
		return -ENOENT;
	}
	if (base->connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
		drm_connector_put(base);
		drm_dev_exit(idx);
		return -ENOENT;
	}
	connector = drm_connector_to_castkms_connector(base);

	args->uapi_major = DRM_CASTKMS_CEC_UAPI_MAJOR;
	args->uapi_minor = DRM_CASTKMS_CEC_UAPI_MINOR;
	args->capabilities = DRM_CASTKMS_CEC_CAP_EDID_PHYS_ADDR;
	args->max_msg_size = CASTKMS_CEC_MAX_MSG_SIZE;
	args->output_index = connector->output_index;
	args->has_adapter = connector->cec ? 1 : 0;

	drm_connector_put(base);
	drm_dev_exit(idx);
	return 0;
}
