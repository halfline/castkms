// SPDX-License-Identifier: GPL-2.0+

#include <linux/build_bug.h>
#include <linux/slab.h>

#include <drm/castkms_drm.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>

#include <media/cec.h>

#include "castkms_capture_authority.h"
#include "castkms_cec_core.h"
#include "castkms_cec_uapi.h"
#include "castkms_connector.h"
#include "castkms_grant.h"

/**
 * struct castkms_cec_uapi_transport - DRM-file CEC transport adapter
 * @connector: Refcounted connector represented by UAPI object IDs
 * @transport_id: File-visible binding identifier
 */
struct castkms_cec_uapi_transport {
	struct drm_connector *connector;
	u32 transport_id;
};

static_assert(sizeof(struct drm_castkms_cec_query_caps) == 40);
static_assert(sizeof(struct drm_castkms_cec_bind_transport) == 48);
static_assert(offsetof(struct drm_castkms_cec_bind_transport, pad0) == 44);
static_assert(sizeof(struct drm_castkms_cec_unbind_transport) == 16);

static struct castkms_cec_output *
cec_uapi_lookup(struct drm_device *dev, struct drm_file *file_priv,
		u32 connector_id, struct drm_connector **out_connector)
{
	struct castkms_connector *connector;
	struct drm_connector *base;

	base = drm_connector_lookup(dev, file_priv, connector_id);
	if (!base)
		return ERR_PTR(-ENOENT);
	if (base->connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
		drm_connector_put(base);
		return ERR_PTR(-ENOENT);
	}

	connector = drm_connector_to_castkms_connector(base);
	if (!connector->cec) {
		drm_connector_put(base);
		return ERR_PTR(-ENOENT);
	}

	*out_connector = base;
	return connector->cec;
}

static struct castkms_cec_output *
cec_uapi_lookup_granted(struct drm_device *dev, struct drm_file *file_priv,
			u32 connector_id, struct drm_connector **out_connector,
			struct castkms_capture_authority **out_authority)
{
	struct castkms_cec_output *output;
	struct drm_connector *connector;
	int ret;

	output = cec_uapi_lookup(dev, file_priv, connector_id, &connector);
	if (IS_ERR(output))
		return output;

	ret = castkms_grant_begin(file_priv, connector,
				  CASTKMS_CAPTURE_AUTHORITY_MANAGE_CEC,
				  out_authority);
	if (ret) {
		drm_connector_put(connector);
		return ERR_PTR(ret);
	}

	*out_connector = connector;
	return output;
}

static void castkms_cec_uapi_release(void *data)
{
	struct castkms_cec_uapi_transport *transport = data;

	drm_connector_put(transport->connector);
	kfree(transport);
}

static const struct castkms_cec_transport_ops castkms_cec_uapi_ops = {
	.release = castkms_cec_uapi_release,
};

static u32 castkms_cec_uapi_state_flags(const struct castkms_cec_state *state)
{
	u32 flags = 0;

	if (state->transport_online)
		flags |= DRM_CASTKMS_CEC_STATE_TRANSPORT_ONLINE;
	if (state->monitor_attached)
		flags |= DRM_CASTKMS_CEC_STATE_MONITOR_ATTACHED;
	if (state->adapter_enabled)
		flags |= DRM_CASTKMS_CEC_STATE_ADAPTER_ENABLED;
	return flags;
}

static int castkms_cec_uapi_get_transport(struct castkms_cec_output *output,
					  struct castkms_capture_authority *authority,
					  u32 transport_id,
					  struct castkms_cec_state *state)
{
	int ret;

	ret = castkms_cec_core_get_state(output, authority, state);
	if (!ret && (u32)state->transport_generation != transport_id)
		ret = -EACCES;
	return ret;
}

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

int castkms_cec_bind_transport_ioctl(struct drm_device *dev, void *data,
				     struct drm_file *file_priv)
{
	struct drm_castkms_cec_bind_transport *args = data;
	struct castkms_cec_uapi_transport *transport;
	struct castkms_capture_authority *authority;
	struct castkms_cec_state state;
	struct castkms_cec_output *output;
	struct castkms_connector *connector;
	struct drm_connector *base;
	int idx;
	int ret;

	if (args->flags || args->reserved)
		return -EINVAL;
	if (!drm_dev_enter(dev, &idx))
		return -ENODEV;

	output = cec_uapi_lookup_granted(dev, file_priv, args->connector_id,
					 &base, &authority);
	if (IS_ERR(output)) {
		ret = PTR_ERR(output);
		goto out_dev;
	}
	connector = drm_connector_to_castkms_connector(base);

	transport = kzalloc_obj(*transport);
	if (!transport) {
		ret = -ENOMEM;
		goto out_authority;
	}
	transport->connector = base;
	drm_connector_get(transport->connector);

	ret = castkms_cec_core_bind(output, authority, &castkms_cec_uapi_ops,
				    transport, &state);
	if (ret) {
		castkms_cec_uapi_release(transport);
		goto out_authority;
	}
	transport->transport_id = (u32)state.transport_generation;

	args->transport_id = transport->transport_id;
	args->transport_generation = state.transport_generation;
	args->state_generation = state.state_generation;
	args->output_index = connector->output_index;
	args->state_flags = castkms_cec_uapi_state_flags(&state);
	args->phys_addr = state.phys_addr;
	args->logical_addr_mask = state.logical_addr_mask;
	args->pad0 = 0;

out_authority:
	castkms_grant_end(authority);
	drm_connector_put(base);
out_dev:
	drm_dev_exit(idx);
	return ret;
}

int castkms_cec_unbind_transport_ioctl(struct drm_device *dev, void *data,
				       struct drm_file *file_priv)
{
	struct drm_castkms_cec_unbind_transport *args = data;
	struct castkms_capture_authority *authority;
	struct castkms_cec_state state;
	struct castkms_cec_output *output;
	struct drm_connector *connector;
	int idx;
	int ret;

	if (args->flags || args->reserved)
		return -EINVAL;
	if (!drm_dev_enter(dev, &idx))
		return -ENODEV;

	output = cec_uapi_lookup_granted(dev, file_priv, args->connector_id,
					 &connector, &authority);
	if (IS_ERR(output)) {
		ret = PTR_ERR(output);
		goto out_dev;
	}
	ret = castkms_cec_uapi_get_transport(output, authority,
					     args->transport_id, &state);
	if (!ret)
		ret = castkms_cec_core_unbind(output, authority,
					      state.transport_generation);

	castkms_grant_end(authority);
	drm_connector_put(connector);
out_dev:
	drm_dev_exit(idx);
	return ret;
}
