// SPDX-License-Identifier: GPL-2.0+

#include <linux/build_bug.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/string.h>

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
 * @dev: Device used for DRM event delivery
 * @file: Grant-bearing DRM file used for event delivery
 * @connector: Refcounted connector represented by UAPI object IDs
 * @transport_id: File-visible binding identifier
 */
struct castkms_cec_uapi_transport {
	struct drm_device *dev;
	struct drm_file *file;
	struct drm_connector *connector;
	u32 transport_id;
};

/* @pending must remain first because DRM event cleanup frees its address. */
struct castkms_cec_uapi_request {
	struct drm_pending_event pending;
	struct drm_castkms_cec_event_tx event;
	struct castkms_cec_request request;
	struct drm_device *dev;
};

static_assert(sizeof(struct drm_castkms_cec_query_caps) == 40);
static_assert(sizeof(struct drm_castkms_cec_bind_transport) == 48);
static_assert(offsetof(struct drm_castkms_cec_bind_transport, pad0) == 44);
static_assert(sizeof(struct drm_castkms_cec_unbind_transport) == 16);
static_assert(sizeof(struct drm_castkms_cec_set_transport_state) == 16);
static_assert(sizeof(struct drm_castkms_cec_tx_complete) == 32);
static_assert(sizeof(struct drm_castkms_cec_receive) == 40);
static_assert(offsetof(struct drm_castkms_cec_receive, pad0) == 35);
static_assert(sizeof(struct drm_castkms_cec_event_tx) == 72);

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

static void castkms_cec_uapi_request_complete(struct castkms_cec_request *request,
					      bool cancelled)
{
	struct castkms_cec_uapi_request *uapi_request = container_of(request,
		struct castkms_cec_uapi_request, request);
	struct drm_device *dev = uapi_request->dev;

	if (cancelled)
		drm_event_cancel_free(dev, &uapi_request->pending);
	else
		drm_send_event(dev, &uapi_request->pending);
}

static void castkms_cec_uapi_request_retire(void *data)
{
	fput(data);
}

static int castkms_cec_uapi_prepare_tx(void *data,
				       const struct castkms_cec_tx *tx,
				       struct castkms_cec_request **request)
{
	struct castkms_cec_uapi_transport *transport = data;
	struct castkms_connector *connector =
		drm_connector_to_castkms_connector(transport->connector);
	struct castkms_cec_uapi_request *uapi_request;
	struct file *active_file;
	int ret;

	*request = NULL;
	active_file = get_file_active(&transport->file->filp);
	if (!active_file)
		return -ENONET;

	uapi_request = kzalloc_obj(*uapi_request);
	if (!uapi_request) {
		fput(active_file);
		return -ENOMEM;
	}

	uapi_request->event.base.type = DRM_CASTKMS_CEC_EVENT_TX;
	uapi_request->event.base.length = sizeof(uapi_request->event);
	uapi_request->event.transport_id = transport->transport_id;
	uapi_request->event.transport_generation = tx->transport_generation;
	uapi_request->event.state_generation = tx->state_generation;
	uapi_request->event.cookie = tx->cookie;
	uapi_request->event.connector_id = transport->connector->base.id;
	uapi_request->event.output_index = connector->output_index;
	uapi_request->event.attempts = tx->attempts;
	uapi_request->event.signal_free_time = tx->signal_free_time;
	uapi_request->event.length = tx->length;
	memcpy(uapi_request->event.msg, tx->msg, tx->length);
	uapi_request->request.complete = castkms_cec_uapi_request_complete;
	uapi_request->request.retire = castkms_cec_uapi_request_retire;
	uapi_request->request.retire_data = active_file;
	uapi_request->dev = transport->dev;

	ret = drm_event_reserve_init(transport->dev, transport->file,
				     &uapi_request->pending,
				     &uapi_request->event.base);
	if (ret) {
		kfree(uapi_request);
		fput(active_file);
		return ret;
	}

	*request = &uapi_request->request;
	return 0;
}

static void castkms_cec_uapi_release(void *data)
{
	struct castkms_cec_uapi_transport *transport = data;

	drm_connector_put(transport->connector);
	kfree(transport);
}

static const struct castkms_cec_transport_ops castkms_cec_uapi_ops = {
	.prepare_tx = castkms_cec_uapi_prepare_tx,
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
	args->capabilities = DRM_CASTKMS_CEC_CAP_ASYNC_TX |
			     DRM_CASTKMS_CEC_CAP_RX_INJECT |
			     DRM_CASTKMS_CEC_CAP_TRANSPORT_STATE |
			     DRM_CASTKMS_CEC_CAP_EDID_PHYS_ADDR;
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
	transport->dev = dev;
	transport->file = file_priv;
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

int castkms_cec_set_transport_state_ioctl(struct drm_device *dev, void *data,
					  struct drm_file *file_priv)
{
	struct drm_castkms_cec_set_transport_state *args = data;
	struct castkms_capture_authority *authority;
	struct castkms_cec_state state;
	struct castkms_cec_output *output;
	struct drm_connector *connector;
	int idx;
	int ret;

	if (args->flags & ~DRM_CASTKMS_CEC_TRANSPORT_ONLINE || args->reserved)
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
		ret = castkms_cec_core_set_online(output, authority,
						  state.transport_generation,
						  args->flags &
						  DRM_CASTKMS_CEC_TRANSPORT_ONLINE);

	castkms_grant_end(authority);
	drm_connector_put(connector);
out_dev:
	drm_dev_exit(idx);
	return ret;
}

int castkms_cec_tx_complete_ioctl(struct drm_device *dev, void *data,
				  struct drm_file *file_priv)
{
	struct drm_castkms_cec_tx_complete *args = data;
	struct castkms_capture_authority *authority;
	struct castkms_cec_state state;
	struct castkms_cec_output *output;
	struct drm_connector *connector;
	int idx;
	int ret;

	if (args->reserved[0] || args->reserved[1] || args->reserved[2] ||
	    !args->status)
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
		ret = castkms_cec_core_tx_complete(output, authority,
						   args->transport_generation,
						   args->cookie, args->status,
						   args->arb_lost_cnt,
						   args->nack_cnt,
						   args->low_drive_cnt,
						   args->error_cnt);

	castkms_grant_end(authority);
	drm_connector_put(connector);
out_dev:
	drm_dev_exit(idx);
	return ret;
}

int castkms_cec_receive_ioctl(struct drm_device *dev, void *data,
			      struct drm_file *file_priv)
{
	struct drm_castkms_cec_receive *args = data;
	struct castkms_capture_authority *authority;
	struct castkms_cec_state state;
	struct castkms_cec_output *output;
	struct drm_connector *connector;
	int idx;
	int ret;

	if (args->flags || args->reserved ||
	    memchr_inv(args->pad0, 0, sizeof(args->pad0)) ||
	    args->length < 1 ||
	    args->length > CASTKMS_CEC_MAX_MSG_SIZE)
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
		ret = castkms_cec_core_receive(output, authority,
					       args->transport_generation,
					       args->msg, args->length);

	castkms_grant_end(authority);
	drm_connector_put(connector);
out_dev:
	drm_dev_exit(idx);
	return ret;
}
