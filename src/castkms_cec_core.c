// SPDX-License-Identifier: GPL-2.0+

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#include <drm/display/drm_hdmi_cec_helper.h>

#include <media/cec.h>

#include <kunit/visibility.h>

#include "castkms_capture_authority.h"
#include "castkms_cec_core.h"
#include "castkms_connector.h"

#define CEC_TX_TIMEOUT_MS 2000

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
 * @lock: Protects the binding, pending transaction, and state fields
 * @transport_wait: Waits for transport callbacks outside @lock
 * @transport_users: Number of active transport callbacks
 * @transport_cleanup: Number of cleanups blocking transaction dispatch
 * @transport: Current transport binding, or NULL
 * @transport_generation: Monotonic binding generation
 * @transport_online: Whether the transport is ready for dispatch
 * @adapter_enabled: Latest CEC adapter enable state
 * @logical_addr_mask: Logical addresses assigned by the CEC core
 * @state_generation: Monotonic state-change generation
 * @next_cookie: Monotonic outbound transaction cookie
 * @pending_cookie: Outstanding transaction cookie, or zero
 * @pending_msg_len: Length of the pending outbound message
 * @pending_attempts: Requested attempt count for the pending message
 * @pending_signal_free_time: Signal-free time for the pending message
 * @pending_msg: Pending outbound CEC message bytes
 * @tx_timeout_work: Completes abandoned transactions
 * @stats_tx_submitted: Transactions submitted to the transport
 * @stats_tx_completed: Successful transmit completions
 * @stats_tx_nack: NACK transmit completions
 * @stats_tx_error: Other transmit errors
 * @stats_tx_timeout: Timed-out transactions
 * @stats_rx: Messages injected by the transport
 * @stats_invalid: Rejected transport requests
 */
struct castkms_cec_output {
	struct castkms_connector *connector;
	spinlock_t lock; /* Protects binding, transaction, and state fields. */
	wait_queue_head_t transport_wait;
	unsigned int transport_users;
	unsigned int transport_cleanup;

	struct castkms_cec_transport *transport;
	u64 transport_generation;
	bool transport_online;

	bool adapter_enabled;
	u16 logical_addr_mask;
	u64 state_generation;

	u64 next_cookie;
	u64 pending_cookie;
	u8 pending_msg_len;
	u8 pending_attempts;
	u32 pending_signal_free_time;
	u8 pending_msg[CASTKMS_CEC_MAX_MSG_SIZE];
	struct delayed_work tx_timeout_work;

	u64 stats_tx_submitted;
	u64 stats_tx_completed;
	u64 stats_tx_nack;
	u64 stats_tx_error;
	u64 stats_tx_timeout;
	u64 stats_rx;
	u64 stats_invalid;

#if IS_ENABLED(CONFIG_KUNIT)
	const struct castkms_cec_test_ops *test_ops;
	void *test_data;
#endif
};

static struct castkms_cec_output *
connector_to_cec(struct drm_connector *connector)
{
	return drm_connector_to_castkms_connector(connector)->cec;
}

static bool cec_abort_pending_transmission(struct castkms_cec_output *output)
{
	lockdep_assert_held(&output->lock);
	if (!output->pending_cookie)
		return false;

	output->pending_cookie = 0;
	output->state_generation++;
	return true;
}

static void cec_transport_user_put(struct castkms_cec_output *output)
{
	unsigned long flags;
	bool idle;

	spin_lock_irqsave(&output->lock, flags);
	if (WARN_ON(!output->transport_users)) {
		spin_unlock_irqrestore(&output->lock, flags);
		return;
	}
	idle = !--output->transport_users;
	spin_unlock_irqrestore(&output->lock, flags);

	if (idle)
		wake_up_all(&output->transport_wait);
}

static void cec_transport_wait_idle(struct castkms_cec_output *output)
{
	wait_event(output->transport_wait,
		   !READ_ONCE(output->transport_users));
}

static void cec_transport_cleanup_done(struct castkms_cec_output *output)
{
	unsigned long flags;

	spin_lock_irqsave(&output->lock, flags);
	WARN_ON(!output->transport_cleanup);
	if (output->transport_cleanup)
		output->transport_cleanup--;
	spin_unlock_irqrestore(&output->lock, flags);
}

static void castkms_cec_core_tx_done(struct castkms_cec_output *output,
				     u8 status, u8 arb_lost_cnt,
				     u8 nack_cnt, u8 low_drive_cnt,
				     u8 error_cnt)
{
#if IS_ENABLED(CONFIG_KUNIT)
	if (output->test_ops) {
		if (output->test_ops->tx_done)
			output->test_ops->tx_done(output->test_data, status,
					      arb_lost_cnt, nack_cnt,
					      low_drive_cnt, error_cnt);
		return;
	}
#endif
	drm_connector_hdmi_cec_transmit_done(&output->connector->base,
					     status, arb_lost_cnt, nack_cnt,
					     low_drive_cnt, error_cnt);
}

static void castkms_cec_core_tx_aborted(struct castkms_cec_output *output)
{
	castkms_cec_core_tx_done(output, CEC_TX_STATUS_ERROR, 0, 0, 0, 1);
}

static void castkms_cec_core_finish_cleanup(struct castkms_cec_output *output,
					    struct castkms_cec_transport *transport,
					    bool aborted, bool release)
{
	cec_transport_wait_idle(output);
	cancel_delayed_work_sync(&output->tx_timeout_work);
	if (aborted)
		castkms_cec_core_tx_aborted(output);
	if (release) {
		transport->ops->release(transport->data);
		kfree(transport);
	}
	cec_transport_cleanup_done(output);
}

static void cec_tx_timeout_work_fn(struct work_struct *work)
{
	struct castkms_cec_output *output =
		container_of(work, struct castkms_cec_output,
			     tx_timeout_work.work);
	unsigned long flags;
	bool need_done = false;

	spin_lock_irqsave(&output->lock, flags);
	if (output->pending_cookie) {
		output->pending_cookie = 0;
		output->stats_tx_timeout++;
		output->state_generation++;
		need_done = true;
	}
	spin_unlock_irqrestore(&output->lock, flags);

	if (need_done)
		castkms_cec_core_tx_aborted(output);
}

static int castkms_cec_adapter_init(struct drm_connector *connector)
{
	return 0;
}

static int castkms_cec_enable(struct drm_connector *connector, bool enable)
{
	struct castkms_cec_output *output = connector_to_cec(connector);
	unsigned long flags;
	bool cancelled = false;

	spin_lock_irqsave(&output->lock, flags);
	if (output->adapter_enabled != enable) {
		output->adapter_enabled = enable;
		output->state_generation++;
		if (!enable)
			cancelled = cec_abort_pending_transmission(output);
	}
	spin_unlock_irqrestore(&output->lock, flags);

	/*
	 * CEC core invokes ->enable() with its adapter mutex held.  Never wait
	 * synchronously for the timeout worker here: that worker may already be
	 * waiting for the same core mutex in cec_transmit_done().
	 */
	if (cancelled)
		cancel_delayed_work(&output->tx_timeout_work);

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

static int castkms_cec_transmit(struct drm_connector *connector, u8 attempts,
				u32 signal_free_time, struct cec_msg *msg)
{
	struct castkms_cec_output *output = connector_to_cec(connector);
	struct castkms_cec_transport *transport;
	struct castkms_cec_request *request = NULL;
	struct castkms_cec_tx tx = {};
	void (*retire)(void *data);
	void *retire_data;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&output->lock, flags);
	transport = output->transport;
	if (!output->adapter_enabled ||
	    !READ_ONCE(output->connector->monitor_attached) ||
	    !transport ||
	    !castkms_capture_authority_is_active(transport->authority) ||
	    !output->transport_online || output->transport_cleanup) {
		spin_unlock_irqrestore(&output->lock, flags);
		return -ENONET;
	}
	if (output->pending_cookie) {
		spin_unlock_irqrestore(&output->lock, flags);
		return -EBUSY;
	}

	output->transport_users++;
	output->next_cookie++;
	output->pending_cookie = output->next_cookie;
	output->pending_msg_len = msg->len;
	output->pending_attempts = attempts;
	output->pending_signal_free_time = signal_free_time;
	memcpy(output->pending_msg, msg->msg, msg->len);

	tx.transport_generation = transport->generation;
	tx.state_generation = output->state_generation;
	tx.cookie = output->pending_cookie;
	tx.signal_free_time = signal_free_time;
	tx.attempts = attempts;
	tx.length = msg->len;
	memcpy(tx.msg, msg->msg, msg->len);
	spin_unlock_irqrestore(&output->lock, flags);

	ret = transport->ops->prepare_tx(transport->data, &tx, &request);
	if (ret || !request || !request->complete) {
		if (!ret)
			ret = -EINVAL;
		spin_lock_irqsave(&output->lock, flags);
		if (output->transport == transport &&
		    output->pending_cookie == tx.cookie)
			output->pending_cookie = 0;
		spin_unlock_irqrestore(&output->lock, flags);
		cec_transport_user_put(output);
		return ret;
	}
	retire = request->retire;
	retire_data = request->retire_data;

	spin_lock_irqsave(&output->lock, flags);
	if (output->transport != transport ||
	    !castkms_capture_authority_is_active(transport->authority) ||
	    transport->generation != tx.transport_generation ||
	    output->pending_cookie != tx.cookie ||
	    output->transport_cleanup) {
		if (output->transport == transport &&
		    output->pending_cookie == tx.cookie)
			output->pending_cookie = 0;
		spin_unlock_irqrestore(&output->lock, flags);
		request->complete(request, true);
		cec_transport_user_put(output);
		if (retire)
			retire(retire_data);
		return -ENONET;
	}
	output->stats_tx_submitted++;
	spin_unlock_irqrestore(&output->lock, flags);

	request->complete(request, false);
	schedule_delayed_work(&output->tx_timeout_work,
			      msecs_to_jiffies(CEC_TX_TIMEOUT_MS));
	cec_transport_user_put(output);
	if (retire)
		retire(retire_data);
	return 0;
}

static const struct drm_connector_hdmi_cec_funcs castkms_cec_funcs = {
	.init = castkms_cec_adapter_init,
	.enable = castkms_cec_enable,
	.log_addr = castkms_cec_log_addr,
	.transmit = castkms_cec_transmit,
};

static void castkms_cec_copy_state(struct castkms_cec_output *output,
				   struct castkms_cec_state *state)
{
	lockdep_assert_held(&output->lock);
	state->transport_generation = output->transport->generation;
	state->state_generation = output->state_generation;
	state->pending_cookie = output->pending_cookie;
	state->stats_tx_submitted = output->stats_tx_submitted;
	state->stats_tx_completed = output->stats_tx_completed;
	state->stats_tx_nack = output->stats_tx_nack;
	state->stats_tx_error = output->stats_tx_error;
	state->stats_tx_timeout = output->stats_tx_timeout;
	state->stats_rx = output->stats_rx;
	state->stats_invalid = output->stats_invalid;
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
	init_waitqueue_head(&output->transport_wait);
	INIT_DELAYED_WORK(&output->tx_timeout_work, cec_tx_timeout_work_fn);
	output->next_cookie = 1;

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
	struct castkms_cec_transport *transport =
		container_of(resource, struct castkms_cec_transport, resource);
	struct castkms_cec_output *output = transport->output;
	unsigned long flags;
	bool aborted;

	(void)status;
	spin_lock_irqsave(&output->lock, flags);
	if (output->transport != transport) {
		spin_unlock_irqrestore(&output->lock, flags);
		return;
	}
	aborted = cec_abort_pending_transmission(output);
	output->transport_online = false;
	output->transport_cleanup++;
	output->state_generation++;
	spin_unlock_irqrestore(&output->lock, flags);

	castkms_cec_core_finish_cleanup(output, transport, aborted, false);
}

static void castkms_cec_resource_revoke(struct castkms_capture_authority_resource *resource,
					int status)
{
	struct castkms_cec_transport *transport =
		container_of(resource, struct castkms_cec_transport, resource);
	struct castkms_cec_output *output = transport->output;
	unsigned long flags;
	bool aborted = false;
	bool attached = false;

	(void)status;
	spin_lock_irqsave(&output->lock, flags);
	if (output->transport == transport) {
		aborted = cec_abort_pending_transmission(output);
		output->transport = NULL;
		output->transport_online = false;
		output->transport_cleanup++;
		output->state_generation++;
		attached = true;
	}
	spin_unlock_irqrestore(&output->lock, flags);

	if (attached) {
		castkms_cec_core_finish_cleanup(output, transport, aborted, true);
	} else {
		transport->ops->release(transport->data);
		kfree(transport);
	}
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
	if (!ops || !ops->prepare_tx || !ops->release || !state)
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
	if (output->transport || output->transport_cleanup) {
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
EXPORT_SYMBOL_IF_KUNIT(castkms_cec_core_bind);

int castkms_cec_core_unbind(struct castkms_cec_output *output,
			    struct castkms_capture_authority *authority,
			    u64 transport_generation)
{
	struct castkms_cec_transport *transport;
	unsigned long flags;
	bool aborted;

	if (!output)
		return -ENOENT;

	spin_lock_irqsave(&output->lock, flags);
	transport = output->transport;
	if (!transport || transport->authority != authority) {
		spin_unlock_irqrestore(&output->lock, flags);
		return -EACCES;
	}
	if (transport->generation != transport_generation) {
		spin_unlock_irqrestore(&output->lock, flags);
		return -ESTALE;
	}
	spin_unlock_irqrestore(&output->lock, flags);

	if (!castkms_capture_authority_unregister_resource(authority,
							   &transport->resource))
		return -EKEYREVOKED;

	spin_lock_irqsave(&output->lock, flags);
	if (WARN_ON(output->transport != transport)) {
		spin_unlock_irqrestore(&output->lock, flags);
		transport->ops->release(transport->data);
		kfree(transport);
		return -EIO;
	}
	aborted = cec_abort_pending_transmission(output);
	output->transport = NULL;
	output->transport_online = false;
	output->transport_cleanup++;
	output->state_generation++;
	spin_unlock_irqrestore(&output->lock, flags);

	castkms_cec_core_finish_cleanup(output, transport, aborted, true);
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_cec_core_unbind);

int castkms_cec_core_set_online(struct castkms_cec_output *output,
				struct castkms_capture_authority *authority,
				u64 transport_generation, bool online)
{
	struct castkms_cec_transport *transport;
	unsigned long flags;
	bool aborted = false;
	bool changed = false;

	if (!output)
		return -ENOENT;

	spin_lock_irqsave(&output->lock, flags);
	transport = output->transport;
	if (!transport || transport->authority != authority) {
		spin_unlock_irqrestore(&output->lock, flags);
		return -EACCES;
	}
	if (transport->generation != transport_generation) {
		spin_unlock_irqrestore(&output->lock, flags);
		return -ESTALE;
	}
	if (output->transport_cleanup) {
		spin_unlock_irqrestore(&output->lock, flags);
		return -EAGAIN;
	}

	if (online != output->transport_online) {
		output->transport_online = online;
		output->state_generation++;
		changed = true;
		if (!online) {
			aborted = cec_abort_pending_transmission(output);
			output->transport_cleanup++;
		}
	}
	spin_unlock_irqrestore(&output->lock, flags);

	if (changed && !online)
		castkms_cec_core_finish_cleanup(output, transport, aborted, false);
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_cec_core_set_online);

int castkms_cec_core_tx_complete(struct castkms_cec_output *output,
				 struct castkms_capture_authority *authority,
				 u64 transport_generation, u64 cookie, u8 status,
				 u8 arb_lost_cnt, u8 nack_cnt, u8 low_drive_cnt,
				 u8 error_cnt)
{
	struct castkms_cec_transport *transport;
	unsigned long flags;
	int ret = 0;

	if (!output)
		return -ENOENT;
	spin_lock_irqsave(&output->lock, flags);
	transport = output->transport;
	if (!transport || transport->authority != authority) {
		output->stats_invalid++;
		ret = -EACCES;
		goto out_unlock;
	}
	if (transport->generation != transport_generation) {
		output->stats_invalid++;
		ret = -ESTALE;
		goto out_unlock;
	}
	if (!output->pending_cookie || output->pending_cookie != cookie) {
		output->stats_invalid++;
		ret = -ENOENT;
		goto out_unlock;
	}

	output->pending_cookie = 0;
	if (status & CEC_TX_STATUS_OK)
		output->stats_tx_completed++;
	else if (status & CEC_TX_STATUS_NACK)
		output->stats_tx_nack++;
	else
		output->stats_tx_error++;
	spin_unlock_irqrestore(&output->lock, flags);

	cancel_delayed_work_sync(&output->tx_timeout_work);
	castkms_cec_core_tx_done(output, status, arb_lost_cnt, nack_cnt,
				 low_drive_cnt, error_cnt);
	return 0;

out_unlock:
	spin_unlock_irqrestore(&output->lock, flags);
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_cec_core_tx_complete);

int castkms_cec_core_receive(struct castkms_cec_output *output,
			     struct castkms_capture_authority *authority,
			     u64 transport_generation, const u8 *message, u8 length)
{
	struct castkms_cec_transport *transport;
	struct cec_msg msg = {};
	unsigned long flags;
	u8 initiator;
	int ret = 0;

	if (!output)
		return -ENOENT;
	if (!message || length < 1 || length > CEC_MAX_MSG_SIZE)
		return -EINVAL;

	spin_lock_irqsave(&output->lock, flags);
	transport = output->transport;
	if (!transport || transport->authority != authority) {
		output->stats_invalid++;
		ret = -EACCES;
		goto out_unlock;
	}
	if (transport->generation != transport_generation) {
		output->stats_invalid++;
		ret = -ESTALE;
		goto out_unlock;
	}
	if (!output->transport_online) {
		output->stats_invalid++;
		ret = -ENONET;
		goto out_unlock;
	}
	if (!READ_ONCE(output->connector->monitor_attached)) {
		output->stats_invalid++;
		ret = -ENOTCONN;
		goto out_unlock;
	}

	initiator = (message[0] >> 4) & 0x0f;
	if (output->logical_addr_mask & BIT(initiator)) {
		output->stats_invalid++;
		ret = -EINVAL;
		goto out_unlock;
	}
	output->stats_rx++;
	spin_unlock_irqrestore(&output->lock, flags);

	msg.len = length;
	memcpy(msg.msg, message, length);
#if IS_ENABLED(CONFIG_KUNIT)
	if (output->test_ops) {
		if (output->test_ops->received)
			output->test_ops->received(output->test_data,
					       message, length);
		return 0;
	}
#endif
	drm_connector_hdmi_cec_received_msg(&output->connector->base, &msg);
	return 0;

out_unlock:
	spin_unlock_irqrestore(&output->lock, flags);
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_cec_core_receive);

int castkms_cec_core_get_state(struct castkms_cec_output *output,
			       struct castkms_capture_authority *authority,
			       struct castkms_cec_state *state)
{
	unsigned long flags;

	if (!output)
		return -ENOENT;
	if (!state)
		return -EINVAL;

	spin_lock_irqsave(&output->lock, flags);
	if (!output->transport ||
	    output->transport->authority != authority) {
		spin_unlock_irqrestore(&output->lock, flags);
		return -EACCES;
	}
	castkms_cec_copy_state(output, state);
	spin_unlock_irqrestore(&output->lock, flags);
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_cec_core_get_state);

void castkms_cec_core_suspend_connector(struct drm_connector *connector)
{
	struct castkms_cec_output *output = connector_to_cec(connector);
	struct castkms_cec_transport *transport;
	unsigned long flags;
	bool aborted;

	if (!output)
		return;

	/* Keep the binding and online preference across monitor reattachment. */
	spin_lock_irqsave(&output->lock, flags);
	transport = output->transport;
	if (!transport) {
		spin_unlock_irqrestore(&output->lock, flags);
		return;
	}
	aborted = cec_abort_pending_transmission(output);
	output->transport_cleanup++;
	output->state_generation++;
	spin_unlock_irqrestore(&output->lock, flags);

	castkms_cec_core_finish_cleanup(output, transport, aborted, false);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_cec_core_suspend_connector);

#if IS_ENABLED(CONFIG_KUNIT)
struct castkms_cec_output *
castkms_cec_core_test_output_create(struct castkms_connector *connector,
				    const struct castkms_cec_test_ops *ops,
				    void *data)
{
	struct castkms_cec_output *output;

	if (!connector || !ops)
		return ERR_PTR(-EINVAL);
	if (connector->cec)
		return ERR_PTR(-EBUSY);

	output = kzalloc_obj(*output);
	if (!output)
		return ERR_PTR(-ENOMEM);
	output->connector = connector;
	spin_lock_init(&output->lock);
	init_waitqueue_head(&output->transport_wait);
	INIT_DELAYED_WORK(&output->tx_timeout_work, cec_tx_timeout_work_fn);
	output->next_cookie = 1;
	output->test_ops = ops;
	output->test_data = data;
	connector->cec = output;
	return output;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_cec_core_test_output_create);

void castkms_cec_core_test_output_destroy(struct castkms_cec_output *output)
{
	if (!output)
		return;
	if (WARN_ON(output->transport || output->transport_users ||
		    output->transport_cleanup))
		return;
	cancel_delayed_work_sync(&output->tx_timeout_work);
	output->connector->cec = NULL;
	kfree(output);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_cec_core_test_output_destroy);

int castkms_cec_core_test_enable(struct castkms_cec_output *output, bool enable)
{
	if (!output)
		return -ENOENT;
	return castkms_cec_enable(&output->connector->base, enable);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_cec_core_test_enable);

int castkms_cec_core_test_transmit(struct castkms_cec_output *output,
				   u8 attempts, u32 signal_free_time,
				   const u8 *message, u8 length)
{
	struct cec_msg msg = {};

	if (!output)
		return -ENOENT;
	if (!message || !length || length > sizeof(msg.msg))
		return -EINVAL;
	msg.len = length;
	memcpy(msg.msg, message, length);
	return castkms_cec_transmit(&output->connector->base, attempts,
				    signal_free_time, &msg);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_cec_core_test_transmit);

void castkms_cec_core_test_timeout(struct castkms_cec_output *output)
{
	if (!output)
		return;
	cancel_delayed_work_sync(&output->tx_timeout_work);
	cec_tx_timeout_work_fn(&output->tx_timeout_work.work);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_cec_core_test_timeout);
#endif
