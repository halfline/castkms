// SPDX-License-Identifier: GPL-2.0+

#include <linux/device.h>
#include <linux/slab.h>

#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#include <drm/display/drm_hdmi_cec_helper.h>

#include <media/cec.h>

#include "castkms_capture_authority.h"
#include "castkms_cec_core.h"
#include "castkms_connector.h"

/**
 * struct castkms_cec_transport - one core CEC transport binding
 * @output: Output served by this transport
 * @authority: Authority which owns the binding
 * @resource: Authority suspension and revocation hook
 * @ops: Transport adapter operations
 * @data: Opaque transport adapter state
 * @generation: Binding generation
 */
struct castkms_cec_transport {
	struct castkms_cec_output *output;
	struct castkms_capture_authority *authority;
	struct castkms_capture_authority_resource resource;
	const struct castkms_cec_transport_ops *ops;
	void *data;
	u64 generation;
};

/**
 * struct castkms_cec_output - per-connector CEC core state
 * @connector: Back-pointer to the owning CastKMS connector
 * @lock: Protects the binding and state fields
 * @transport: Current transport binding, or NULL
 * @transport_generation: Monotonic binding generation
 * @transport_online: Whether the transport is ready for dispatch
 * @adapter_enabled: Latest CEC adapter enable state
 * @logical_addr_mask: Logical addresses assigned by the CEC core
 * @state_generation: Monotonic state-change generation
 */
struct castkms_cec_output {
	struct castkms_connector *connector;
	spinlock_t lock; /* Protects binding and state fields. */

	struct castkms_cec_transport *transport;
	u64 transport_generation;
	bool transport_online;

	bool adapter_enabled;
	u16 logical_addr_mask;
	u64 state_generation;
};

static struct castkms_cec_output *
connector_to_cec(struct drm_connector *connector)
{
	return drm_connector_to_castkms_connector(connector)->cec;
}

static int castkms_cec_adapter_init(struct drm_connector *connector)
{
	return 0;
}

static int castkms_cec_enable(struct drm_connector *connector, bool enable)
{
	struct castkms_cec_output *output = connector_to_cec(connector);
	unsigned long flags;

	spin_lock_irqsave(&output->lock, flags);
	if (output->adapter_enabled != enable) {
		output->adapter_enabled = enable;
		output->state_generation++;
	}
	spin_unlock_irqrestore(&output->lock, flags);

	return 0;
}

static int castkms_cec_log_addr(struct drm_connector *connector, u8 logical_addr)
{
	struct castkms_cec_output *output = connector_to_cec(connector);
	unsigned long flags;

	spin_lock_irqsave(&output->lock, flags);
	if (logical_addr == CEC_LOG_ADDR_INVALID)
		output->logical_addr_mask = 0;
	else
		output->logical_addr_mask |= BIT(logical_addr);
	output->state_generation++;
	spin_unlock_irqrestore(&output->lock, flags);

	return 0;
}

static const struct drm_connector_hdmi_cec_funcs castkms_cec_funcs = {
	.init = castkms_cec_adapter_init,
	.enable = castkms_cec_enable,
	.log_addr = castkms_cec_log_addr,
};

static void castkms_cec_copy_state(struct castkms_cec_output *output,
				   struct castkms_cec_state *state)
{
	lockdep_assert_held(&output->lock);
	state->transport_generation = output->transport->generation;
	state->state_generation = output->state_generation;
	state->phys_addr =
		output->connector->base.display_info.source_physical_address;
	state->logical_addr_mask = output->logical_addr_mask;
	state->transport_online = output->transport_online;
	state->monitor_attached =
		READ_ONCE(output->connector->monitor_attached);
	state->adapter_enabled = output->adapter_enabled;
}

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
	spin_lock_init(&output->lock);

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

static void castkms_cec_resource_suspend(struct castkms_capture_authority_resource *resource,
					 int status)
{
	(void)resource;
	(void)status;
}

static void castkms_cec_resource_revoke(struct castkms_capture_authority_resource *resource,
					int status)
{
	struct castkms_cec_transport *transport =
		container_of(resource, struct castkms_cec_transport, resource);
	struct castkms_cec_output *output = transport->output;
	unsigned long flags;

	(void)status;
	spin_lock_irqsave(&output->lock, flags);
	if (output->transport == transport) {
		output->transport = NULL;
		output->transport_online = false;
		output->state_generation++;
	}
	spin_unlock_irqrestore(&output->lock, flags);

	transport->ops->release(transport->data);
	kfree(transport);
}

static const struct castkms_capture_authority_resource_ops
castkms_cec_resource_ops = {
	.suspend = castkms_cec_resource_suspend,
	.revoke = castkms_cec_resource_revoke,
};

int castkms_cec_core_bind(struct castkms_cec_output *output,
			  struct castkms_capture_authority *authority,
			  const struct castkms_cec_transport_ops *ops, void *data,
			  struct castkms_cec_state *state)
{
	struct castkms_cec_transport *transport;
	unsigned long flags;
	int ret;

	if (!output)
		return -ENOENT;
	if (!ops || !ops->release || !state)
		return -EINVAL;

	transport = kzalloc_obj(*transport);
	if (!transport)
		return -ENOMEM;
	transport->output = output;
	transport->authority = authority;
	transport->ops = ops;
	transport->data = data;

	ret = castkms_capture_authority_register_resource(authority,
							  &transport->resource,
							  &castkms_cec_resource_ops);
	if (ret)
		goto out_free;

	spin_lock_irqsave(&output->lock, flags);
	if (output->transport) {
		ret = -EBUSY;
		spin_unlock_irqrestore(&output->lock, flags);
		goto out_unregister;
	}

	output->transport_generation++;
	transport->generation = output->transport_generation;
	output->transport = transport;
	output->transport_online = false;
	output->state_generation++;
	castkms_cec_copy_state(output, state);
	spin_unlock_irqrestore(&output->lock, flags);
	return 0;

out_unregister:
	castkms_capture_authority_unregister_resource(authority,
						      &transport->resource);
out_free:
	kfree(transport);
	return ret;
}

void castkms_cec_core_suspend_connector(struct drm_connector *connector)
{
	(void)connector;
}
