// SPDX-License-Identifier: GPL-2.0+

#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_file.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#include <drm/display/drm_hdmi_cec_helper.h>

#include <media/cec.h>

#include <drm/castkms_drm.h>

#include "castkms_capture_authority.h"
#include "castkms_cec.h"
#include "castkms_connector.h"
#include "castkms_drv.h"
#include "castkms_grant.h"

#define CEC_TX_TIMEOUT_MS 2000

/* --- Internal helpers --- */

static struct castkms_cec_output *
connector_to_cec(struct drm_connector *connector)
{
	struct castkms_connector *castkms_conn =
		drm_connector_to_castkms_connector(connector);

	return castkms_conn->cec;
}

static struct castkms_cec_output *
cec_lookup(struct drm_device *dev, struct drm_file *file_priv,
	   u32 connector_id, struct drm_connector **out_connector)
{
	struct drm_connector *connector;
	struct castkms_connector *castkms_conn;
	struct castkms_cec_output *cec_out;

	connector = drm_connector_lookup(dev, file_priv, connector_id);
	if (!connector)
		return ERR_PTR(-ENOENT);

	if (connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
		drm_connector_put(connector);
		return ERR_PTR(-ENOENT);
	}

	castkms_conn = drm_connector_to_castkms_connector(connector);
	cec_out = castkms_conn->cec;
	if (!cec_out) {
		drm_connector_put(connector);
		return ERR_PTR(-ENOENT);
	}

	*out_connector = connector;
	return cec_out;
}

static struct castkms_cec_output *
cec_lookup_granted(struct drm_device *dev, struct drm_file *file_priv,
		   u32 connector_id, struct drm_connector **out_connector,
		   struct castkms_capture_authority **out_authority)
{
	struct castkms_cec_output *cec_out;
	struct drm_connector *connector;
	int ret;

	cec_out = cec_lookup(dev, file_priv, connector_id, &connector);
	if (IS_ERR(cec_out))
		return cec_out;

	ret = castkms_grant_begin(file_priv, connector,
				  CASTKMS_CAPTURE_AUTHORITY_MANAGE_CEC,
				  out_authority);
	if (ret) {
		drm_connector_put(connector);
		return ERR_PTR(ret);
	}

	*out_connector = connector;
	return cec_out;
}

static bool cec_abort_pending_locked(struct castkms_cec_output *cec_out)
{
	if (!cec_out->pending_cookie)
		return false;

	cec_out->pending_cookie = 0;
	cec_out->state_generation++;
	return true;
}

static void cec_transport_user_put(struct castkms_cec_output *cec_out)
{
	unsigned long flags;
	bool idle;

	spin_lock_irqsave(&cec_out->lock, flags);
	if (WARN_ON(!cec_out->transport_users)) {
		spin_unlock_irqrestore(&cec_out->lock, flags);
		return;
	}
	idle = !--cec_out->transport_users;
	spin_unlock_irqrestore(&cec_out->lock, flags);

	if (idle)
		wake_up_all(&cec_out->transport_wait);
}

static void cec_transport_user_release(struct castkms_cec_output *cec_out,
				       struct file *active_file)
{
	/*
	 * Final fput may enter holder postclose and wait for transport cleanup.
	 * Leave the callback-use barrier first so that cleanup cannot wait on the
	 * callback which is performing that final fput.
	 */
	cec_transport_user_put(cec_out);
	fput(active_file);
}

static void cec_transport_wait_idle(struct castkms_cec_output *cec_out)
{
	wait_event(cec_out->transport_wait,
		   !READ_ONCE(cec_out->transport_users));
}

static void cec_transport_cleanup_done(struct castkms_cec_output *cec_out)
{
	unsigned long flags;

	spin_lock_irqsave(&cec_out->lock, flags);
	WARN_ON(!cec_out->transport_cleanup);
	if (cec_out->transport_cleanup)
		cec_out->transport_cleanup--;
	spin_unlock_irqrestore(&cec_out->lock, flags);
}

static void cec_tx_timeout_work_fn(struct work_struct *work)
{
	struct castkms_cec_output *cec_out =
		container_of(work, struct castkms_cec_output,
			     tx_timeout_work.work);
	struct drm_connector *connector;
	unsigned long flags;
	bool need_done = false;

	spin_lock_irqsave(&cec_out->lock, flags);
	if (cec_out->pending_cookie) {
		cec_out->pending_cookie = 0;
		cec_out->stats_tx_timeout++;
		cec_out->state_generation++;
		need_done = true;
	}
	spin_unlock_irqrestore(&cec_out->lock, flags);

	if (need_done) {
		connector = &cec_out->connector->base;
		drm_connector_hdmi_cec_transmit_done(connector,
						     CEC_TX_STATUS_ERROR,
						     0, 0, 0, 1);
	}
}

/* --- CEC adapter callbacks --- */

static int castkms_cec_adapter_init(struct drm_connector *connector)
{
	return 0;
}

static int castkms_cec_enable(struct drm_connector *connector, bool enable)
{
	struct castkms_cec_output *cec_out = connector_to_cec(connector);
	unsigned long flags;
	bool canceled = false;

	spin_lock_irqsave(&cec_out->lock, flags);
	if (cec_out->adapter_enabled != enable) {
		cec_out->adapter_enabled = enable;
		cec_out->state_generation++;
		if (!enable)
			canceled = cec_abort_pending_locked(cec_out);
	}
	spin_unlock_irqrestore(&cec_out->lock, flags);

	/*
	 * CEC core invokes ->enable() with its adapter mutex held.  Never wait
	 * synchronously for the timeout worker here: that worker may already be
	 * waiting for the same core mutex in cec_transmit_done().  CEC core cancels
	 * its in-progress transaction itself immediately after ->enable(false)
	 * returns, and a racing timeout completion is explicitly tolerated by the
	 * core.
	 */
	if (canceled)
		cancel_delayed_work(&cec_out->tx_timeout_work);

	return 0;
}

static int castkms_cec_log_addr(struct drm_connector *connector, u8 logical_addr)
{
	struct castkms_cec_output *cec_out = connector_to_cec(connector);
	unsigned long flags;

	spin_lock_irqsave(&cec_out->lock, flags);
	if (logical_addr == CEC_LOG_ADDR_INVALID)
		cec_out->logical_addr_mask = 0;
	else
		cec_out->logical_addr_mask |= BIT(logical_addr);
	cec_out->state_generation++;
	spin_unlock_irqrestore(&cec_out->lock, flags);

	return 0;
}

static int castkms_cec_transmit(struct drm_connector *connector, u8 attempts,
				u32 signal_free_time, struct cec_msg *msg)
{
	struct castkms_cec_output *cec_out = connector_to_cec(connector);
	struct drm_device *dev = connector->dev;
	struct castkms_capture_authority *transport_authority;
	struct drm_pending_event *pending_event;
	struct drm_castkms_cec_event_tx *tx_event;
	struct drm_file *transport_file;
	struct file *active_file;
	u64 transport_generation;
	u64 cookie;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&cec_out->lock, flags);

	if (!cec_out->adapter_enabled ||
	    !READ_ONCE(cec_out->connector->monitor_attached) ||
	    !cec_out->transport_authority ||
	    !castkms_capture_authority_is_active(cec_out->transport_authority) ||
	    !cec_out->transport_file ||
	    !cec_out->transport_online || cec_out->transport_cleanup) {
		spin_unlock_irqrestore(&cec_out->lock, flags);
		return -ENONET;
	}

	if (cec_out->pending_cookie) {
		spin_unlock_irqrestore(&cec_out->lock, flags);
		return -EBUSY;
	}
	active_file = get_file_active(&cec_out->transport_file->filp);
	if (!active_file) {
		spin_unlock_irqrestore(&cec_out->lock, flags);
		return -ENONET;
	}
	cec_out->transport_users++;

	cec_out->next_cookie++;
	cec_out->pending_cookie = cec_out->next_cookie;
	cec_out->pending_msg_len = msg->len;
	cec_out->pending_attempts = attempts;
	cec_out->pending_signal_free_time = signal_free_time;
	memcpy(cec_out->pending_msg, msg->msg, msg->len);
	transport_authority = cec_out->transport_authority;
	transport_file = cec_out->transport_file;
	transport_generation = cec_out->transport_generation;
	cookie = cec_out->pending_cookie;

	spin_unlock_irqrestore(&cec_out->lock, flags);

	pending_event = kzalloc(sizeof(*pending_event) + sizeof(*tx_event),
				GFP_KERNEL);
	if (!pending_event) {
		spin_lock_irqsave(&cec_out->lock, flags);
		if (cec_out->transport_authority == transport_authority &&
		    cec_out->transport_generation == transport_generation &&
		    cec_out->pending_cookie == cookie)
			cec_out->pending_cookie = 0;
		spin_unlock_irqrestore(&cec_out->lock, flags);
		cec_transport_user_release(cec_out, active_file);
		return -ENOMEM;
	}

	tx_event = (void *)(pending_event + 1);
	tx_event->base.type = DRM_CASTKMS_CEC_EVENT_TX;
	tx_event->base.length = sizeof(*tx_event);

	spin_lock_irqsave(&cec_out->lock, flags);

	if (cec_out->transport_authority != transport_authority ||
	    !castkms_capture_authority_is_active(transport_authority) ||
	    cec_out->transport_file != transport_file ||
	    cec_out->transport_generation != transport_generation ||
	    cec_out->pending_cookie != cookie ||
	    cec_out->transport_cleanup) {
		spin_unlock_irqrestore(&cec_out->lock, flags);
		kfree(pending_event);
		cec_transport_user_release(cec_out, active_file);
		return -ENONET;
	}

	tx_event->transport_id = cec_out->transport_id;
	tx_event->transport_generation = cec_out->transport_generation;
	tx_event->state_generation = cec_out->state_generation;
	tx_event->cookie = cec_out->pending_cookie;
	tx_event->connector_id = connector->base.id;
	tx_event->output_index = cec_out->connector->output_index;
	tx_event->attempts = attempts;
	tx_event->signal_free_time = signal_free_time;
	tx_event->length = msg->len;
	memcpy(tx_event->msg, msg->msg, msg->len);

	pending_event->event = &tx_event->base;

	ret = drm_event_reserve_init(dev, transport_file,
				     pending_event, &tx_event->base);
	if (ret) {
		cec_out->pending_cookie = 0;
		spin_unlock_irqrestore(&cec_out->lock, flags);
		kfree(pending_event);
		cec_transport_user_release(cec_out, active_file);
		return ret;
	}
	cec_out->stats_tx_submitted++;
	spin_unlock_irqrestore(&cec_out->lock, flags);

	drm_send_event(dev, pending_event);

	schedule_delayed_work(&cec_out->tx_timeout_work,
			      msecs_to_jiffies(CEC_TX_TIMEOUT_MS));
	cec_transport_user_release(cec_out, active_file);

	return 0;
}

static const struct drm_connector_hdmi_cec_funcs castkms_cec_funcs = {
	.init = castkms_cec_adapter_init,
	.enable = castkms_cec_enable,
	.log_addr = castkms_cec_log_addr,
	.transmit = castkms_cec_transmit,
};

/* --- Physical-address refresh --- */

void castkms_cec_refresh_connector_state(struct drm_connector *connector)
{
	struct castkms_cec_output *cec_out = connector_to_cec(connector);
	struct castkms_connector *castkms_conn;
	bool should_be_valid;
	unsigned long flags;

	if (!cec_out)
		return;

	castkms_conn = drm_connector_to_castkms_connector(connector);

	spin_lock_irqsave(&cec_out->lock, flags);
	should_be_valid = READ_ONCE(castkms_conn->monitor_attached) &&
			  cec_out->transport_authority &&
			  castkms_capture_authority_is_active(cec_out->transport_authority) &&
			  cec_out->transport_file &&
			  cec_out->transport_online &&
			  connector->display_info.source_physical_address !=
			  CEC_PHYS_ADDR_INVALID;
	spin_unlock_irqrestore(&cec_out->lock, flags);

	if (should_be_valid)
		drm_connector_cec_phys_addr_set(connector);
	else
		drm_connector_cec_phys_addr_invalidate(connector);
}

/* --- Per-connector init --- */

void castkms_cec_connector_init(struct castkms_connector *castkms_conn)
{
	struct drm_connector *connector = &castkms_conn->base;
	struct drm_device *dev = connector->dev;
	struct castkms_cec_output *cec_out;
	char name[64];
	int ret;

	cec_out = drmm_kzalloc(dev, sizeof(*cec_out), GFP_KERNEL);
	if (!cec_out) {
		drm_warn(dev, "castkms: failed to allocate CEC output %u\n",
			 castkms_conn->output_index);
		return;
	}

	cec_out->connector = castkms_conn;
	spin_lock_init(&cec_out->lock);
	init_waitqueue_head(&cec_out->transport_wait);
	INIT_DELAYED_WORK(&cec_out->tx_timeout_work, cec_tx_timeout_work_fn);
	cec_out->next_cookie = 1;

	snprintf(name, sizeof(name), "castkms-%d-output-%u",
		 dev->primary->index, castkms_conn->output_index);

	ret = drmm_connector_hdmi_cec_register(connector, &castkms_cec_funcs,
					       name, 1, dev->dev);
	if (ret) {
		drm_warn(dev, "castkms: CEC register failed for output %u: %d\n",
			 castkms_conn->output_index, ret);
		return;
	}

	castkms_conn->cec = cec_out;
}

/* --- Device-level init --- */

int castkms_cec_init(struct castkms_device *castkmsdev)
{
	struct drm_device *dev = &castkmsdev->drm;
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;

	drm_connector_list_iter_begin(dev, &iter);
	drm_for_each_connector_iter(connector, &iter) {
		struct castkms_connector *castkms_conn;

		if (connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK)
			continue;

		castkms_conn = drm_connector_to_castkms_connector(connector);
		castkms_cec_connector_init(castkms_conn);
	}
	drm_connector_list_iter_end(&iter);

	return 0;
}

/* --- Transport UAPI --- */

int castkms_cec_query_caps(struct drm_device *dev, void *data,
			   struct drm_file *file_priv)
{
	struct drm_castkms_cec_query_caps *args = data;
	struct drm_connector *connector;
	struct castkms_connector *castkms_conn;
	int idx;

	if (args->flags || args->reserved)
		return -EINVAL;

	if (!drm_dev_enter(dev, &idx))
		return -ENODEV;

	connector = drm_connector_lookup(dev, file_priv, args->connector_id);
	if (!connector) {
		drm_dev_exit(idx);
		return -ENOENT;
	}

	if (connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
		drm_connector_put(connector);
		drm_dev_exit(idx);
		return -ENOENT;
	}

	castkms_conn = drm_connector_to_castkms_connector(connector);

	args->uapi_major = DRM_CASTKMS_CEC_UAPI_MAJOR;
	args->uapi_minor = DRM_CASTKMS_CEC_UAPI_MINOR;
	args->capabilities = DRM_CASTKMS_CEC_CAP_ASYNC_TX |
			     DRM_CASTKMS_CEC_CAP_RX_INJECT |
			     DRM_CASTKMS_CEC_CAP_TRANSPORT_STATE |
			     DRM_CASTKMS_CEC_CAP_EDID_PHYS_ADDR;
	args->max_msg_size = CEC_MAX_MSG_SIZE;
	args->output_index = castkms_conn->output_index;
	args->has_adapter = castkms_conn->cec ? 1 : 0;

	drm_connector_put(connector);
	drm_dev_exit(idx);

	return 0;
}

int castkms_cec_bind_transport(struct drm_device *dev, void *data,
			       struct drm_file *file_priv)
{
	struct drm_castkms_cec_bind_transport *args = data;
	struct castkms_capture_authority *authority;
	struct drm_connector *connector;
	struct castkms_connector *castkms_conn;
	struct castkms_cec_output *cec_out;
	unsigned long flags;
	int idx;

	if (args->flags || args->reserved)
		return -EINVAL;

	if (!drm_dev_enter(dev, &idx))
		return -ENODEV;

	cec_out = cec_lookup_granted(dev, file_priv, args->connector_id,
				     &connector, &authority);
	if (IS_ERR(cec_out)) {
		drm_dev_exit(idx);
		return PTR_ERR(cec_out);
	}
	castkms_conn = drm_connector_to_castkms_connector(connector);

	spin_lock_irqsave(&cec_out->lock, flags);
	if (cec_out->transport_authority || cec_out->transport_cleanup) {
		spin_unlock_irqrestore(&cec_out->lock, flags);
		castkms_grant_end(authority);
		drm_connector_put(connector);
		drm_dev_exit(idx);
		return -EBUSY;
	}

	cec_out->transport_authority = authority;
	castkms_capture_authority_get(authority);
	cec_out->transport_file = file_priv;
	cec_out->transport_generation++;
	cec_out->transport_id = cec_out->transport_generation & 0xFFFFFFFF;
	cec_out->transport_online = false;
	cec_out->state_generation++;

	args->transport_id = cec_out->transport_id;
	args->transport_generation = cec_out->transport_generation;
	args->state_generation = cec_out->state_generation;
	args->output_index = castkms_conn->output_index;
	args->state_flags = 0;
	if (READ_ONCE(castkms_conn->monitor_attached))
		args->state_flags |= DRM_CASTKMS_CEC_STATE_MONITOR_ATTACHED;
	if (cec_out->adapter_enabled)
		args->state_flags |= DRM_CASTKMS_CEC_STATE_ADAPTER_ENABLED;
	args->phys_addr = connector->display_info.source_physical_address;
	args->logical_addr_mask = cec_out->logical_addr_mask;

	spin_unlock_irqrestore(&cec_out->lock, flags);
	castkms_grant_end(authority);
	drm_connector_put(connector);
	drm_dev_exit(idx);

	return 0;
}

int castkms_cec_unbind_transport(struct drm_device *dev, void *data,
				 struct drm_file *file_priv)
{
	struct drm_castkms_cec_unbind_transport *args = data;
	struct castkms_capture_authority *authority;
	struct castkms_capture_authority *transport_authority;
	struct drm_connector *connector;
	struct castkms_cec_output *cec_out;
	unsigned long flags;
	bool aborted = false;
	int idx;

	if (args->flags || args->reserved)
		return -EINVAL;

	if (!drm_dev_enter(dev, &idx))
		return -ENODEV;

	cec_out = cec_lookup_granted(dev, file_priv, args->connector_id,
				     &connector, &authority);
	if (IS_ERR(cec_out)) {
		drm_dev_exit(idx);
		return PTR_ERR(cec_out);
	}

	spin_lock_irqsave(&cec_out->lock, flags);
	if (cec_out->transport_authority != authority ||
	    cec_out->transport_id != args->transport_id) {
		spin_unlock_irqrestore(&cec_out->lock, flags);
		castkms_grant_end(authority);
		drm_connector_put(connector);
		drm_dev_exit(idx);
		return -EACCES;
	}

	aborted = cec_abort_pending_locked(cec_out);
	cec_out->transport_cleanup++;
	transport_authority = cec_out->transport_authority;
	cec_out->transport_authority = NULL;
	cec_out->transport_file = NULL;
	cec_out->transport_online = false;
	cec_out->state_generation++;
	spin_unlock_irqrestore(&cec_out->lock, flags);

	cec_transport_wait_idle(cec_out);
	cancel_delayed_work_sync(&cec_out->tx_timeout_work);

	if (aborted)
		drm_connector_hdmi_cec_transmit_done(connector,
						     CEC_TX_STATUS_ERROR,
						     0, 0, 0, 1);

	castkms_cec_refresh_connector_state(connector);
	castkms_capture_authority_put(transport_authority);
	cec_transport_cleanup_done(cec_out);
	castkms_grant_end(authority);

	drm_connector_put(connector);
	drm_dev_exit(idx);

	return 0;
}

int castkms_cec_set_transport_state(struct drm_device *dev, void *data,
				    struct drm_file *file_priv)
{
	struct drm_castkms_cec_set_transport_state *args = data;
	struct castkms_capture_authority *authority;
	struct drm_connector *connector;
	struct castkms_cec_output *cec_out;
	unsigned long flags;
	bool aborted = false;
	bool changed = false;
	int idx;

	if (args->flags & ~DRM_CASTKMS_CEC_TRANSPORT_ONLINE)
		return -EINVAL;
	if (args->reserved)
		return -EINVAL;

	if (!drm_dev_enter(dev, &idx))
		return -ENODEV;

	cec_out = cec_lookup_granted(dev, file_priv, args->connector_id,
				     &connector, &authority);
	if (IS_ERR(cec_out)) {
		drm_dev_exit(idx);
		return PTR_ERR(cec_out);
	}

	spin_lock_irqsave(&cec_out->lock, flags);
	if (cec_out->transport_authority != authority ||
	    cec_out->transport_id != args->transport_id) {
		spin_unlock_irqrestore(&cec_out->lock, flags);
		castkms_grant_end(authority);
		drm_connector_put(connector);
		drm_dev_exit(idx);
		return -EACCES;
	}
	if (cec_out->transport_cleanup) {
		spin_unlock_irqrestore(&cec_out->lock, flags);
		castkms_grant_end(authority);
		drm_connector_put(connector);
		drm_dev_exit(idx);
		return -EAGAIN;
	}

	if (args->flags & DRM_CASTKMS_CEC_TRANSPORT_ONLINE) {
		if (!cec_out->transport_online) {
			cec_out->transport_online = true;
			cec_out->state_generation++;
			changed = true;
		}
	} else {
		if (cec_out->transport_online) {
			cec_out->transport_online = false;
			cec_out->state_generation++;
			aborted = cec_abort_pending_locked(cec_out);
			cec_out->transport_cleanup++;
			changed = true;
		}
	}
	spin_unlock_irqrestore(&cec_out->lock, flags);

	if (changed && !(args->flags & DRM_CASTKMS_CEC_TRANSPORT_ONLINE)) {
		cec_transport_wait_idle(cec_out);
		cancel_delayed_work_sync(&cec_out->tx_timeout_work);
		if (aborted)
			drm_connector_hdmi_cec_transmit_done(connector,
							     CEC_TX_STATUS_ERROR,
							     0, 0, 0, 1);
	}

	if (changed)
		castkms_cec_refresh_connector_state(connector);
	if (changed && !(args->flags & DRM_CASTKMS_CEC_TRANSPORT_ONLINE))
		cec_transport_cleanup_done(cec_out);

	castkms_grant_end(authority);
	drm_connector_put(connector);
	drm_dev_exit(idx);

	return 0;
}

int castkms_cec_tx_complete(struct drm_device *dev, void *data,
			    struct drm_file *file_priv)
{
	struct drm_castkms_cec_tx_complete *args = data;
	struct castkms_capture_authority *authority;
	struct drm_connector *connector;
	struct castkms_cec_output *cec_out;
	unsigned long flags;
	int ret = 0;
	int idx;

	if (args->reserved[0] || args->reserved[1] || args->reserved[2])
		return -EINVAL;
	if (!args->status)
		return -EINVAL;

	if (!drm_dev_enter(dev, &idx))
		return -ENODEV;

	cec_out = cec_lookup_granted(dev, file_priv, args->connector_id,
				     &connector, &authority);
	if (IS_ERR(cec_out)) {
		drm_dev_exit(idx);
		return PTR_ERR(cec_out);
	}

	spin_lock_irqsave(&cec_out->lock, flags);
	if (cec_out->transport_authority != authority ||
	    cec_out->transport_id != args->transport_id) {
		cec_out->stats_invalid++;
		ret = -EACCES;
		goto out_unlock;
	}

	if (cec_out->transport_generation != args->transport_generation) {
		cec_out->stats_invalid++;
		ret = -ESTALE;
		goto out_unlock;
	}

	if (!cec_out->pending_cookie ||
	    cec_out->pending_cookie != args->cookie) {
		cec_out->stats_invalid++;
		ret = -ENOENT;
		goto out_unlock;
	}

	cec_out->pending_cookie = 0;
	if (args->status & CEC_TX_STATUS_OK)
		cec_out->stats_tx_completed++;
	else if (args->status & CEC_TX_STATUS_NACK)
		cec_out->stats_tx_nack++;
	else
		cec_out->stats_tx_error++;
	spin_unlock_irqrestore(&cec_out->lock, flags);

	cancel_delayed_work_sync(&cec_out->tx_timeout_work);

	drm_connector_hdmi_cec_transmit_done(connector, args->status,
					     args->arb_lost_cnt,
					     args->nack_cnt,
					     args->low_drive_cnt,
					     args->error_cnt);

	goto out;

out_unlock:
	spin_unlock_irqrestore(&cec_out->lock, flags);
out:
	castkms_grant_end(authority);
	drm_connector_put(connector);
	drm_dev_exit(idx);

	return ret;
}

int castkms_cec_receive(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct drm_castkms_cec_receive *args = data;
	struct castkms_capture_authority *authority;
	struct drm_connector *connector;
	struct castkms_cec_output *cec_out;
	struct cec_msg msg = {};
	unsigned long flags;
	u8 initiator;
	int ret = 0;
	int idx;

	if (args->flags || args->reserved)
		return -EINVAL;
	if (args->length < 1 || args->length > CEC_MAX_MSG_SIZE)
		return -EINVAL;

	if (!drm_dev_enter(dev, &idx))
		return -ENODEV;

	cec_out = cec_lookup_granted(dev, file_priv, args->connector_id,
				     &connector, &authority);
	if (IS_ERR(cec_out)) {
		drm_dev_exit(idx);
		return PTR_ERR(cec_out);
	}

	spin_lock_irqsave(&cec_out->lock, flags);
	if (cec_out->transport_authority != authority ||
	    cec_out->transport_id != args->transport_id) {
		cec_out->stats_invalid++;
		ret = -EACCES;
		goto out_unlock;
	}

	if (cec_out->transport_generation != args->transport_generation) {
		cec_out->stats_invalid++;
		ret = -ESTALE;
		goto out_unlock;
	}

	if (!cec_out->transport_online) {
		cec_out->stats_invalid++;
		ret = -ENONET;
		goto out_unlock;
	}
	if (!READ_ONCE(cec_out->connector->monitor_attached)) {
		cec_out->stats_invalid++;
		ret = -ENOTCONN;
		goto out_unlock;
	}

	initiator = (args->msg[0] >> 4) & 0x0f;
	if (cec_out->logical_addr_mask & BIT(initiator)) {
		cec_out->stats_invalid++;
		ret = -EINVAL;
		goto out_unlock;
	}

	cec_out->stats_rx++;
	spin_unlock_irqrestore(&cec_out->lock, flags);

	msg.len = args->length;
	memcpy(msg.msg, args->msg, args->length);

	drm_connector_hdmi_cec_received_msg(connector, &msg);
	goto out;

out_unlock:
	spin_unlock_irqrestore(&cec_out->lock, flags);
out:
	castkms_grant_end(authority);
	drm_connector_put(connector);
	drm_dev_exit(idx);

	return ret;
}

int castkms_cec_get_state(struct drm_device *dev, void *data,
			  struct drm_file *file_priv)
{
	struct drm_castkms_cec_get_state *args = data;
	struct castkms_capture_authority *authority;
	struct drm_connector *connector;
	struct castkms_connector *castkms_conn;
	struct castkms_cec_output *cec_out;
	unsigned long flags;
	int idx;

	if (args->flags || args->reserved)
		return -EINVAL;

	if (!drm_dev_enter(dev, &idx))
		return -ENODEV;

	cec_out = cec_lookup_granted(dev, file_priv, args->connector_id,
				     &connector, &authority);
	if (IS_ERR(cec_out)) {
		drm_dev_exit(idx);
		return PTR_ERR(cec_out);
	}
	castkms_conn = drm_connector_to_castkms_connector(connector);

	spin_lock_irqsave(&cec_out->lock, flags);
	if (cec_out->transport_authority != authority ||
	    cec_out->transport_id != args->transport_id) {
		spin_unlock_irqrestore(&cec_out->lock, flags);
		castkms_grant_end(authority);
		drm_connector_put(connector);
		drm_dev_exit(idx);
		return -EACCES;
	}

	args->transport_generation = cec_out->transport_generation;
	args->state_generation = cec_out->state_generation;
	args->state_flags = 0;
	if (cec_out->transport_online)
		args->state_flags |= DRM_CASTKMS_CEC_STATE_TRANSPORT_ONLINE;
	if (READ_ONCE(castkms_conn->monitor_attached))
		args->state_flags |= DRM_CASTKMS_CEC_STATE_MONITOR_ATTACHED;
	if (cec_out->adapter_enabled)
		args->state_flags |= DRM_CASTKMS_CEC_STATE_ADAPTER_ENABLED;
	args->phys_addr = connector->display_info.source_physical_address;
	args->logical_addr_mask = cec_out->logical_addr_mask;
	args->pending_cookie = cec_out->pending_cookie;
	args->output_index = castkms_conn->output_index;

	args->stats_tx_submitted = cec_out->stats_tx_submitted;
	args->stats_tx_completed = cec_out->stats_tx_completed;
	args->stats_tx_nack = cec_out->stats_tx_nack;
	args->stats_tx_error = cec_out->stats_tx_error;
	args->stats_tx_timeout = cec_out->stats_tx_timeout;
	args->stats_rx = cec_out->stats_rx;
	args->stats_invalid = cec_out->stats_invalid;

	spin_unlock_irqrestore(&cec_out->lock, flags);

	castkms_grant_end(authority);
	drm_connector_put(connector);
	drm_dev_exit(idx);

	return 0;
}

/* --- Grant revoke/final-close cleanup --- */

void castkms_cec_unbind_authority(struct castkms_capture_authority *authority)
{
	struct drm_connector *connector = castkms_capture_authority_connector(authority);
	struct castkms_connector *castkms_conn =
		drm_connector_to_castkms_connector(connector);
	struct castkms_cec_output *cec_out = castkms_conn->cec;
	struct castkms_capture_authority *transport_authority = NULL;
	unsigned long flags;
	bool aborted = false;

	if (!cec_out)
		return;

	spin_lock_irqsave(&cec_out->lock, flags);
	if (cec_out->transport_authority == authority) {
		aborted = cec_abort_pending_locked(cec_out);
		cec_out->transport_cleanup++;
		transport_authority = cec_out->transport_authority;
		cec_out->transport_authority = NULL;
		cec_out->transport_file = NULL;
		cec_out->transport_online = false;
		cec_out->state_generation++;
	}
	spin_unlock_irqrestore(&cec_out->lock, flags);

	if (!transport_authority)
		return;

	cec_transport_wait_idle(cec_out);
	cancel_delayed_work_sync(&cec_out->tx_timeout_work);
	if (aborted)
		drm_connector_hdmi_cec_transmit_done(connector,
						     CEC_TX_STATUS_ERROR,
						     0, 0, 0, 1);

	castkms_cec_refresh_connector_state(connector);
	castkms_capture_authority_put(transport_authority);
	cec_transport_cleanup_done(cec_out);
}

void castkms_cec_suspend_authority(struct castkms_capture_authority *authority)
{
	struct drm_connector *connector = castkms_capture_authority_connector(authority);
	struct castkms_connector *castkms_conn =
		drm_connector_to_castkms_connector(connector);
	struct castkms_cec_output *cec_out = castkms_conn->cec;
	unsigned long flags;
	bool aborted = false;
	bool suspended = false;

	if (!cec_out)
		return;

	spin_lock_irqsave(&cec_out->lock, flags);
	if (cec_out->transport_authority == authority) {
		aborted = cec_abort_pending_locked(cec_out);
		cec_out->transport_online = false;
		cec_out->transport_cleanup++;
		cec_out->state_generation++;
		suspended = true;
	}
	spin_unlock_irqrestore(&cec_out->lock, flags);

	if (!suspended)
		return;

	cec_transport_wait_idle(cec_out);
	cancel_delayed_work_sync(&cec_out->tx_timeout_work);
	if (aborted)
		drm_connector_hdmi_cec_transmit_done(connector,
						     CEC_TX_STATUS_ERROR,
						     0, 0, 0, 1);
	castkms_cec_refresh_connector_state(connector);
	cec_transport_cleanup_done(cec_out);
}

void castkms_cec_suspend_connector(struct drm_connector *connector)
{
	struct castkms_cec_output *cec_out = connector_to_cec(connector);
	unsigned long flags;
	bool aborted;

	if (!cec_out)
		return;

	/*
	 * A monitor detach may belong to a different authority from a CEC-only
	 * transport.  Block event preparation while the attachment disappears,
	 * then retire any transaction which was already pending.  Keep the
	 * userspace transport binding and online preference so reattachment can
	 * restore the physical address without requiring a new capability.
	 */
	spin_lock_irqsave(&cec_out->lock, flags);
	if (!cec_out->transport_authority) {
		spin_unlock_irqrestore(&cec_out->lock, flags);
		castkms_cec_refresh_connector_state(connector);
		return;
	}
	aborted = cec_abort_pending_locked(cec_out);
	cec_out->transport_cleanup++;
	cec_out->state_generation++;
	spin_unlock_irqrestore(&cec_out->lock, flags);

	cec_transport_wait_idle(cec_out);
	cancel_delayed_work_sync(&cec_out->tx_timeout_work);
	if (aborted)
		drm_connector_hdmi_cec_transmit_done(connector,
						     CEC_TX_STATUS_ERROR,
						     0, 0, 0, 1);
	castkms_cec_refresh_connector_state(connector);
	cec_transport_cleanup_done(cec_out);
}
