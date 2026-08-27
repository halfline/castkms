// SPDX-License-Identifier: GPL-2.0+

#include <linux/device.h>
#include <linux/slab.h>

#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#include <drm/display/drm_hdmi_cec_helper.h>

#include "castkms_cec_core.h"
#include "castkms_connector.h"

/**
 * struct castkms_cec_output - per-connector CEC core state
 * @connector: Back-pointer to the owning CastKMS connector
 */
struct castkms_cec_output {
	struct castkms_connector *connector;
};

static int castkms_cec_adapter_init(struct drm_connector *connector)
{
	return 0;
}

static int castkms_cec_enable(struct drm_connector *connector, bool enable)
{
	return 0;
}

static int castkms_cec_log_addr(struct drm_connector *connector, u8 logical_addr)
{
	return 0;
}

static const struct drm_connector_hdmi_cec_funcs castkms_cec_funcs = {
	.init = castkms_cec_adapter_init,
	.enable = castkms_cec_enable,
	.log_addr = castkms_cec_log_addr,
};

int castkms_cec_core_connector_init(struct castkms_connector *connector)
{
	struct drm_connector *base = &connector->base;
	struct drm_device *dev = base->dev;
	struct castkms_cec_output *output;
	char name[64];
	int ret;

	output = drmm_kzalloc(dev, sizeof(*output), GFP_KERNEL);
	if (!output) {
		drm_warn(dev, "castkms: failed to allocate CEC output %u\n",
			 connector->output_index);
		return -ENOMEM;
	}

	output->connector = connector;

	snprintf(name, sizeof(name), "%s-output-%u",
		 dev_name(dev->dev), connector->output_index);
	ret = drmm_connector_hdmi_cec_register(base, &castkms_cec_funcs,
					       name, 1, dev->dev);
	if (ret) {
		drm_err(dev, "castkms: CEC register failed for output %u: %d\n",
			connector->output_index, ret);
		return ret;
	}

	connector->cec = output;
	return 0;
}

int castkms_cec_core_init(struct drm_device *dev)
{
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;
	int ret = 0;

	drm_connector_list_iter_begin(dev, &iter);
	drm_for_each_connector_iter(connector, &iter) {
		struct castkms_connector *castkms_connector;

		if (connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK)
			continue;
		castkms_connector = drm_connector_to_castkms_connector(connector);
		ret = castkms_cec_core_connector_init(castkms_connector);
		if (ret)
			break;
	}
	drm_connector_list_iter_end(&iter);
	return ret;
}

void castkms_cec_core_suspend_connector(struct drm_connector *connector)
{
	(void)connector;
}
