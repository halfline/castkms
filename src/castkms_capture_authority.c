// SPDX-License-Identifier: GPL-2.0-only

#include <linux/completion.h>
#include <linux/err.h>
#include <linux/kref.h>
#include <linux/limits.h>
#include <linux/slab.h>
#include <linux/xarray.h>

#include <drm/drm_auth.h>
#include <drm/drm_atomic.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_file.h>
#include <drm/drm_managed.h>

#include <kunit/visibility.h>

#include "castkms_capture_authority.h"
#include "castkms_cec.h"
#include "castkms_connector.h"
#include "castkms_drv.h"

/**
 * struct castkms_capture_authority - kernel-native capture authorization
 * @ref: Lifetime held by clients, the registry, attachments, and streams
 * @device: Owning CastKMS device
 * @connector: Refcounted connector scoped by this authority
 * @bound_master: Refcounted DRM master for a normal authority, or NULL
 * @ops: Optional owner integration hooks
 * @data: Opaque owner data passed to @ops
 * @lock: Serializes authorization checks with terminal revocation
 * @resource_lock: Serializes suspension and terminal resource cleanup
 * @cleanup_done: Completed after terminal resource cleanup
 * @registry_id: Internal registry identity; never exposed through UAPI
 * @rights: Immutable CASTKMS_CAPTURE_AUTHORITY_* mask
 * @master_cleanup_handled: Latest master-drop cleanup applied
 * @revoke_status: Stable terminal status
 * @administrative: Authority follows safe content across master handoffs
 * @revoked: Permanently inert
 * @cleanup_started: Exactly one caller owns terminal cleanup
 * @registered: Present in the device authority registry
 */
struct castkms_capture_authority {
	struct kref ref;
	struct castkms_device *device;
	struct drm_connector *connector;
	struct drm_master *bound_master;
	const struct castkms_capture_authority_ops *ops;
	void *data;
	struct mutex lock; /* Serializes use with terminal revocation. */
	struct mutex resource_lock; /* Serializes resource cleanup. */
	struct completion cleanup_done;
	u32 registry_id;
	u32 rights;
	u64 master_cleanup_handled;
	int revoke_status;
	bool administrative;
	bool revoked;
	bool cleanup_started;
	bool registered;
};

static bool castkms_capture_authority_device_is_shutdown(
	const struct castkms_device *castkmsdev)
{
	return READ_ONCE(castkmsdev->authorities_shutdown);
}

static void castkms_capture_authority_mark_device_shutdown(
	struct castkms_device *castkmsdev)
{
	WRITE_ONCE(castkmsdev->authorities_shutdown, true);
}

VISIBLE_IF_KUNIT enum castkms_capture_authority_state
castkms_capture_authority_resolve_state(
	bool permanently_revoked, bool device_shutdown, bool administrative,
	const void *bound_master, const void *current_master,
	bool master_active, bool connector_ready, bool content_safe)
{
	if (permanently_revoked || device_shutdown)
		return CASTKMS_CAPTURE_AUTHORITY_REVOKED;
	if (!master_active || !current_master)
		return CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_NO_MASTER;
	if (!administrative && current_master != bound_master)
		return CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_OTHER_MASTER;
	if (!connector_ready)
		return CASTKMS_CAPTURE_AUTHORITY_PENDING;
	if (!content_safe)
		return CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_FOREIGN_CONTENT;

	return CASTKMS_CAPTURE_AUTHORITY_ACTIVE;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_authority_resolve_state);

bool castkms_capture_owner_is_current(
	const struct drm_master *capture_owner,
	const struct drm_master *current_master)
{
	return current_master && capture_owner == current_master;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_owner_is_current);

bool castkms_capture_owner_is_active_current(
	struct drm_device *dev, const struct drm_master *capture_owner)
{
	struct castkms_device *castkmsdev =
		drm_device_to_castkms_device(dev);
	unsigned long flags;
	bool is_current;

	spin_lock_irqsave(&castkmsdev->capture_master_lock, flags);
	is_current = castkmsdev->capture_master_active &&
		     castkms_capture_owner_is_current(
			     capture_owner, castkmsdev->capture_master);
	spin_unlock_irqrestore(&castkmsdev->capture_master_lock, flags);

	return is_current;
}

bool castkms_capture_output_content_is_safe_locked(
	const struct castkms_output *output)
{
	lockdep_assert_held(&output->lock);

	return output->capture.active && !output->capture_owner_updating &&
	       castkms_capture_owner_is_active_current(
		       output->crtc.dev, output->capture_owner);
}

static void castkms_capture_authority_release(struct kref *ref)
{
	struct castkms_capture_authority *authority =
		container_of(ref, struct castkms_capture_authority, ref);
	const struct castkms_capture_authority_ops *ops = authority->ops;
	void *data = authority->data;

	if (authority->bound_master)
		drm_master_put(&authority->bound_master);
	drm_connector_put(authority->connector);
	mutex_destroy(&authority->resource_lock);
	mutex_destroy(&authority->lock);
	if (ops && ops->release)
		ops->release(authority, data);
	kfree(authority);
}

void castkms_capture_authority_get(
	struct castkms_capture_authority *authority)
{
	kref_get(&authority->ref);
}

void castkms_capture_authority_put(
	struct castkms_capture_authority *authority)
{
	if (authority)
		kref_put(&authority->ref, castkms_capture_authority_release);
}

bool castkms_capture_authority_is_revoked(
	const struct castkms_capture_authority *authority)
{
	/* Pairs with revoke's release store before reading @revoke_status. */
	return smp_load_acquire(&authority->revoked);
}

int castkms_capture_authority_lifetime_status(
	const struct castkms_capture_authority *authority)
{
	if (castkms_capture_authority_is_revoked(authority))
		return READ_ONCE(authority->revoke_status) ?: -EKEYREVOKED;
	if (castkms_capture_authority_device_is_shutdown(authority->device))
		return -ENODEV;

	return 0;
}

struct drm_connector *castkms_capture_authority_connector(
	struct castkms_capture_authority *authority)
{
	return authority->connector;
}

u32 castkms_capture_authority_rights(
	const struct castkms_capture_authority *authority)
{
	return authority->rights;
}

bool castkms_capture_authority_is_administrative(
	const struct castkms_capture_authority *authority)
{
	return authority->administrative;
}

bool castkms_capture_authority_has_rights(
	const struct castkms_capture_authority *authority, u32 rights)
{
	return (authority->rights & rights) == rights;
}

static int castkms_capture_authority_status(
	const struct castkms_capture_authority *authority)
{
	struct castkms_device *castkmsdev = authority->device;
	unsigned long flags;
	int status = 0;

	status = castkms_capture_authority_lifetime_status(authority);
	if (status)
		return status;
	if (authority->administrative)
		return 0;

	spin_lock_irqsave(&castkmsdev->capture_master_lock, flags);
	if (!castkmsdev->capture_master_active ||
	    castkmsdev->capture_master != authority->bound_master)
		status = -EAGAIN;
	spin_unlock_irqrestore(&castkmsdev->capture_master_lock, flags);

	return status;
}

bool castkms_capture_authority_is_active(
	const struct castkms_capture_authority *authority)
{
	return !castkms_capture_authority_status(authority);
}

int castkms_capture_authority_get_state(
	const struct castkms_capture_authority *authority,
	enum castkms_capture_authority_state *state)
{
	struct castkms_device *castkmsdev = authority->device;
	struct castkms_output *output = NULL;
	struct drm_master *current_master;
	unsigned long flags;
	bool connector_ready;
	bool content_safe = false;
	bool master_active;
	int ret;

	connector_ready = castkms_connector_is_attached(authority->connector);
	if (connector_ready) {
		ret = castkms_connector_get_routed_output(authority->connector,
							  &output);
		if (ret)
			return ret;
	}
	connector_ready = !!output;

	if (output)
		spin_lock_irqsave(&output->lock, flags);
	else
		local_irq_save(flags);
	spin_lock(&castkmsdev->capture_master_lock);
	current_master = castkmsdev->capture_master;
	master_active = castkmsdev->capture_master_active;
	if (output)
		content_safe = !output->capture_owner_updating &&
			       output->capture.active && current_master &&
			       castkms_capture_owner_is_current(
				       output->capture_owner, current_master);
	*state = castkms_capture_authority_resolve_state(
		castkms_capture_authority_is_revoked(authority),
		castkms_capture_authority_device_is_shutdown(castkmsdev),
		authority->administrative, authority->bound_master,
		current_master, master_active, connector_ready, content_safe);
	spin_unlock(&castkmsdev->capture_master_lock);
	if (output)
		spin_unlock_irqrestore(&output->lock, flags);
	else
		local_irq_restore(flags);

	return 0;
}

int castkms_capture_authority_state_status(
	const struct castkms_capture_authority *authority,
	enum castkms_capture_authority_state state)
{
	switch (state) {
	case CASTKMS_CAPTURE_AUTHORITY_ACTIVE:
		return 0;
	case CASTKMS_CAPTURE_AUTHORITY_PENDING:
		return -ENOLINK;
	case CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_NO_MASTER:
	case CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_OTHER_MASTER:
		return -EAGAIN;
	case CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_FOREIGN_CONTENT:
		return -ESTALE;
	case CASTKMS_CAPTURE_AUTHORITY_REVOKED:
		if (castkms_capture_authority_is_revoked(authority))
			return READ_ONCE(authority->revoke_status) ?: -EKEYREVOKED;
		if (castkms_capture_authority_device_is_shutdown(authority->device))
			return -ENODEV;
		return -EKEYREVOKED;
	default:
		return -EIO;
	}
}

int castkms_capture_authority_capture_status_locked(
	const struct castkms_capture_authority *authority,
	const struct castkms_output *output)
{
	struct castkms_device *castkmsdev = authority->device;
	struct drm_master *current_master;
	int status;

	lockdep_assert_held(&output->lock);
	status = castkms_capture_authority_lifetime_status(authority);
	if (status)
		return status;
	status = 0;

	spin_lock(&castkmsdev->capture_master_lock);
	current_master = castkmsdev->capture_master;
	if (!castkmsdev->capture_master_active || !current_master)
		status = -EAGAIN;
	else if (!authority->administrative &&
		 current_master != authority->bound_master)
		status = -EAGAIN;
	else if (!castkms_connector_is_attached_fast(authority->connector))
		status = -ENOTCONN;
	else if (!output->capture.active)
		status = -ENOLINK;
	else if (output->capture_owner_updating ||
		 !castkms_capture_owner_is_current(output->capture_owner,
						  current_master))
		status = -ESTALE;
	spin_unlock(&castkmsdev->capture_master_lock);

	return status;
}

int castkms_capture_authority_capture_status(
	const struct castkms_capture_authority *authority,
	struct castkms_output *output)
{
	unsigned long flags;
	int status;

	status = castkms_capture_authority_status(authority);
	if (status)
		return status;

	spin_lock_irqsave(&output->lock, flags);
	status = castkms_capture_authority_capture_status_locked(authority,
							    output);
	spin_unlock_irqrestore(&output->lock, flags);

	return status;
}

u64 castkms_capture_authority_stream_generation(
	const struct castkms_capture_authority *authority)
{
	struct castkms_device *castkmsdev = authority->device;
	unsigned long flags;
	u64 generation;

	spin_lock_irqsave(&castkmsdev->capture_master_lock, flags);
	generation = castkmsdev->capture_master_cleanup_sequence;
	spin_unlock_irqrestore(&castkmsdev->capture_master_lock, flags);

	return generation;
}

bool castkms_capture_authority_generation_is_stale(
	u64 stream_generation, u64 cleanup_generation)
{
	return stream_generation < cleanup_generation;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_authority_generation_is_stale);

VISIBLE_IF_KUNIT bool
castkms_capture_authority_stream_generation_is_current(
	u64 stream_generation, u64 cleanup_generation)
{
	return stream_generation == cleanup_generation;
}
EXPORT_SYMBOL_IF_KUNIT(
	castkms_capture_authority_stream_generation_is_current);

int castkms_capture_authority_stream_status_only(
	const struct castkms_capture_authority *authority,
	u64 stream_generation)
{
	struct castkms_device *castkmsdev = authority->device;
	unsigned long flags;
	int status;

	status = castkms_capture_authority_lifetime_status(authority);
	if (status)
		return status;
	status = 0;

	spin_lock_irqsave(&castkmsdev->capture_master_lock, flags);
	if (!castkms_capture_authority_stream_generation_is_current(
		    stream_generation,
		    castkmsdev->capture_master_cleanup_sequence))
		status = -EAGAIN;
	else if (!authority->administrative &&
		 (!castkmsdev->capture_master_active ||
		  castkmsdev->capture_master != authority->bound_master))
		status = -EAGAIN;
	spin_unlock_irqrestore(&castkmsdev->capture_master_lock, flags);

	return status;
}

int castkms_capture_authority_stream_status_locked(
	const struct castkms_capture_authority *authority,
	const struct castkms_output *output, u64 stream_generation)
{
	struct castkms_device *castkmsdev = authority->device;
	struct drm_master *current_master;
	int status;

	lockdep_assert_held(&output->lock);
	status = castkms_capture_authority_lifetime_status(authority);
	if (status)
		return status;
	status = 0;

	spin_lock(&castkmsdev->capture_master_lock);
	current_master = castkmsdev->capture_master;
	if (!castkms_capture_authority_stream_generation_is_current(
		    stream_generation,
		    castkmsdev->capture_master_cleanup_sequence))
		status = -EAGAIN;
	else if (!castkmsdev->capture_master_active || !current_master)
		status = -EAGAIN;
	else if (!authority->administrative &&
		 current_master != authority->bound_master)
		status = -EAGAIN;
	else if (!castkms_connector_is_attached_fast(authority->connector))
		status = -ENOTCONN;
	else if (!output->capture.active)
		status = -ENOLINK;
	else if (output->capture_owner_updating ||
		 !castkms_capture_owner_is_current(output->capture_owner,
						  current_master))
		status = -ESTALE;
	spin_unlock(&castkmsdev->capture_master_lock);

	return status;
}

int castkms_capture_authority_stream_status(
	const struct castkms_capture_authority *authority,
	struct castkms_output *output, u64 stream_generation)
{
	unsigned long flags;
	int status;

	spin_lock_irqsave(&output->lock, flags);
	status = castkms_capture_authority_stream_status_locked(
		authority, output, stream_generation);
	spin_unlock_irqrestore(&output->lock, flags);

	return status;
}

int castkms_capture_authority_begin(
	struct castkms_capture_authority *authority,
	struct drm_connector *connector, u32 rights)
{
	int status;

	if (!authority)
		return -EACCES;

	mutex_lock(&authority->lock);
	status = castkms_capture_authority_status(authority);
	if (!status && !castkms_capture_authority_has_rights(authority, rights))
		status = -EACCES;
	if (!status && connector && authority->connector != connector)
		status = -EACCES;
	if (status)
		mutex_unlock(&authority->lock);

	return status;
}

int castkms_capture_authority_begin_output(
	struct castkms_capture_authority *authority,
	struct castkms_output *output, u32 rights)
{
	struct drm_connector *connector;
	struct castkms_output *routed_output;
	int status;

	if (!authority)
		return -EACCES;
	connector = authority->connector;
	status = castkms_connector_get_routed_output(connector, &routed_output);
	if (status)
		return status;
	if (routed_output != output)
		return -EACCES;

	status = castkms_capture_authority_begin(authority, NULL, rights);
	if (status)
		return status;
	if (!castkms_connector_is_attached(connector))
		status = -ENOTCONN;
	else
		status = castkms_capture_authority_capture_status(authority, output);
	if (status)
		castkms_capture_authority_end(authority);

	return status;
}

void castkms_capture_authority_end(
	struct castkms_capture_authority *authority)
{
	mutex_unlock(&authority->lock);
}

static bool castkms_capture_authority_conflicts(
	struct castkms_device *castkmsdev, struct drm_connector *connector,
	u32 rights)
{
	struct castkms_capture_authority *authority;
	unsigned long id;

	if (!(rights & CASTKMS_CAPTURE_AUTHORITY_MANAGE_ATTACHMENT))
		return false;

	xa_for_each(&castkmsdev->authorities, id, authority) {
		if (authority->connector == connector &&
		    castkms_capture_authority_has_rights(
			    authority,
			    CASTKMS_CAPTURE_AUTHORITY_MANAGE_ATTACHMENT) &&
		    !castkms_capture_authority_is_revoked(authority))
			return true;
	}

	return false;
}

struct castkms_capture_authority *
castkms_capture_authority_create(
	struct castkms_device *castkmsdev, struct drm_connector *connector,
	struct drm_master *bound_master, u32 rights, bool administrative,
	const struct castkms_capture_authority_ops *ops, void *data)
{
	struct castkms_capture_authority *authority;
	unsigned long flags;
	int ret;

	if (!rights || rights & ~CASTKMS_CAPTURE_AUTHORITY_RIGHTS_MASK)
		return ERR_PTR(-EINVAL);
	if (!connector ||
	    connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK)
		return ERR_PTR(-ENOENT);
	if (!administrative && !bound_master)
		return ERR_PTR(-EACCES);

	spin_lock_irqsave(&castkmsdev->capture_master_lock, flags);
	if (castkms_capture_authority_device_is_shutdown(castkmsdev))
		ret = -ENODEV;
	else
		ret = 0;
	spin_unlock_irqrestore(&castkmsdev->capture_master_lock, flags);
	if (ret)
		return ERR_PTR(ret);

	authority = kzalloc_obj(*authority);
	if (!authority)
		return ERR_PTR(-ENOMEM);

	kref_init(&authority->ref);
	authority->device = castkmsdev;
	authority->connector = connector;
	drm_connector_get(authority->connector);
	authority->bound_master = bound_master ?
		drm_master_get(bound_master) : NULL;
	authority->rights = rights;
	authority->administrative = administrative;
	authority->ops = ops;
	authority->data = data;
	mutex_init(&authority->lock);
	mutex_init(&authority->resource_lock);
	init_completion(&authority->cleanup_done);

	spin_lock_irqsave(&castkmsdev->capture_master_lock, flags);
	authority->master_cleanup_handled =
		castkmsdev->capture_master_cleanup_sequence;
	spin_unlock_irqrestore(&castkmsdev->capture_master_lock, flags);

	mutex_lock(&castkmsdev->authority_registry_lock);
	if (castkms_capture_authority_device_is_shutdown(castkmsdev)) {
		ret = -ENODEV;
		goto out_unlock;
	}
	if (castkms_capture_authority_conflicts(castkmsdev, connector, rights)) {
		ret = -EBUSY;
		goto out_unlock;
	}

	castkms_capture_authority_get(authority);
	ret = xa_alloc_cyclic(&castkmsdev->authorities,
				      &authority->registry_id, authority,
				      XA_LIMIT(1, INT_MAX),
				      &castkmsdev->next_authority_id, GFP_KERNEL);
	/* xa_alloc_cyclic() returns 1 when allocation succeeds after wrapping. */
	if (ret < 0) {
		castkms_capture_authority_put(authority);
		goto out_unlock;
	}
	authority->registered = true;
	mutex_unlock(&castkmsdev->authority_registry_lock);

	return authority;

out_unlock:
	mutex_unlock(&castkmsdev->authority_registry_lock);
	/* The caller still owns @data when construction fails. */
	authority->ops = NULL;
	castkms_capture_authority_put(authority);
	return ERR_PTR(ret);
}

static void castkms_capture_authority_unregister(
	struct castkms_capture_authority *authority)
{
	struct castkms_device *castkmsdev = authority->device;
	bool put_registry = false;

	mutex_lock(&castkmsdev->authority_registry_lock);
	if (authority->registered &&
	    xa_load(&castkmsdev->authorities, authority->registry_id) == authority) {
		xa_erase(&castkmsdev->authorities, authority->registry_id);
		authority->registered = false;
		put_registry = true;
	}
	mutex_unlock(&castkmsdev->authority_registry_lock);

	if (put_registry)
		castkms_capture_authority_put(authority);
}

static void castkms_capture_authority_stop_streams(
	struct castkms_capture_authority *authority, int status)
{
	if (authority->ops && authority->ops->stop_streams)
		authority->ops->stop_streams(authority, status, authority->data);
}

static void castkms_capture_authority_stop_streams_before(
	struct castkms_capture_authority *authority, u64 before_generation,
	int status)
{
	if (authority->ops && authority->ops->stop_streams_before)
		authority->ops->stop_streams_before(
			authority, before_generation, status, authority->data);
}

void castkms_capture_authority_revoke(
	struct castkms_capture_authority *authority, int status)
{
	bool cleanup = false;
	bool detached;

	if (status >= 0)
		status = -EKEYREVOKED;

	mutex_lock(&authority->lock);
	if (!authority->revoked) {
		authority->revoke_status = status;
		/* Publish the terminal status before lockless hot-path checks fail. */
		smp_store_release(&authority->revoked, true);
	}
	if (!authority->cleanup_started) {
		authority->cleanup_started = true;
		cleanup = true;
	}
	status = authority->revoke_status;
	mutex_unlock(&authority->lock);

	if (!cleanup) {
		wait_for_completion(&authority->cleanup_done);
		return;
	}

	mutex_lock(&authority->resource_lock);
	castkms_capture_authority_stop_streams(authority, status);
	castkms_cec_unbind_authority(authority);
	detached = castkms_connector_detach_authority(authority);
	mutex_unlock(&authority->resource_lock);

	/* Never acquire a foreign authority lock while holding this one. */
	if (detached)
		castkms_capture_authority_stop_connector_streams(
			authority->connector, authority, -ENOTCONN);
	if (authority->ops && authority->ops->revoked)
		authority->ops->revoked(authority, status, authority->data);
	complete_all(&authority->cleanup_done);
	castkms_capture_authority_unregister(authority);
}

static void castkms_capture_authority_reconcile(
	struct castkms_capture_authority *authority,
	u64 master_cleanup_sequence)
{
	enum castkms_capture_authority_state state;
	bool force_master_cleanup;
	int status;

	mutex_lock(&authority->resource_lock);
	if (castkms_capture_authority_get_state(authority, &state)) {
		mutex_unlock(&authority->resource_lock);
		return;
	}
	if (state == CASTKMS_CAPTURE_AUTHORITY_REVOKED) {
		mutex_unlock(&authority->resource_lock);
		return;
	}

	mutex_lock(&authority->lock);
	force_master_cleanup =
		authority->master_cleanup_handled < master_cleanup_sequence;
	if (force_master_cleanup)
		authority->master_cleanup_handled = master_cleanup_sequence;
	mutex_unlock(&authority->lock);

	status = castkms_capture_authority_state_status(authority, state);
	if (force_master_cleanup)
		castkms_capture_authority_stop_streams_before(
			authority, master_cleanup_sequence, -EAGAIN);
	if (!authority->administrative &&
	    (force_master_cleanup ||
	     state == CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_NO_MASTER ||
	     state == CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_OTHER_MASTER))
		castkms_cec_suspend_authority(authority);
	if (authority->ops && authority->ops->state_changed)
		authority->ops->state_changed(authority, state, status,
					      authority->data);
	mutex_unlock(&authority->resource_lock);
}

static void castkms_capture_authority_master_work_fn(struct work_struct *work)
{
	struct castkms_device *castkmsdev =
		container_of(work, struct castkms_device, capture_master_work);
	struct castkms_capture_authority *authority;
	unsigned long flags;
	unsigned long id;
	u64 master_cleanup_sequence;
	u64 transition;
	u64 current_transition;

	for (;;) {
		spin_lock_irqsave(&castkmsdev->capture_master_lock, flags);
		transition = castkmsdev->capture_master_transition;
		master_cleanup_sequence =
			castkmsdev->capture_master_cleanup_sequence;
		spin_unlock_irqrestore(&castkmsdev->capture_master_lock, flags);

		id = 0;
		for (;;) {
			mutex_lock(&castkmsdev->authority_registry_lock);
			authority = xa_find(&castkmsdev->authorities, &id,
					    ULONG_MAX, XA_PRESENT);
			if (authority)
				castkms_capture_authority_get(authority);
			mutex_unlock(&castkmsdev->authority_registry_lock);
			if (!authority)
				break;

			castkms_capture_authority_reconcile(
				authority, master_cleanup_sequence);
			castkms_capture_authority_put(authority);
			if (id == ULONG_MAX)
				break;
			id++;
		}

		spin_lock_irqsave(&castkmsdev->capture_master_lock, flags);
		current_transition = castkmsdev->capture_master_transition;
		spin_unlock_irqrestore(&castkmsdev->capture_master_lock, flags);
		if (current_transition == transition)
			break;
		cond_resched();
	}
}

static void castkms_capture_authority_device_fini(
	struct drm_device *dev, void *data)
{
	struct castkms_device *castkmsdev = data;
	struct drm_master *master;
	unsigned long flags;

	castkms_capture_authority_mark_device_shutdown(castkmsdev);
	cancel_work_sync(&castkmsdev->capture_master_work);

	spin_lock_irqsave(&castkmsdev->capture_master_lock, flags);
	master = castkmsdev->capture_master;
	castkmsdev->capture_master = NULL;
	castkmsdev->capture_master_file = NULL;
	castkmsdev->capture_master_active = false;
	spin_unlock_irqrestore(&castkmsdev->capture_master_lock, flags);
	if (master)
		drm_master_put(&master);

	WARN_ON(!xa_empty(&castkmsdev->authorities));
	xa_destroy(&castkmsdev->authorities);
	mutex_destroy(&castkmsdev->authority_registry_lock);
}

int castkms_capture_authority_device_init(struct castkms_device *castkmsdev)
{
	mutex_init(&castkmsdev->authority_registry_lock);
	xa_init_flags(&castkmsdev->authorities, XA_FLAGS_ALLOC);
	castkmsdev->next_authority_id = 1;
	spin_lock_init(&castkmsdev->capture_master_lock);
	INIT_WORK(&castkmsdev->capture_master_work,
		  castkms_capture_authority_master_work_fn);

	return drmm_add_action_or_reset(&castkmsdev->drm,
					castkms_capture_authority_device_fini,
					castkmsdev);
}

struct drm_master *castkms_capture_authority_current_master_get(
	struct drm_device *dev)
{
	struct castkms_device *castkmsdev =
		drm_device_to_castkms_device(dev);
	struct drm_master *master = NULL;
	unsigned long flags;

	spin_lock_irqsave(&castkmsdev->capture_master_lock, flags);
	if (castkmsdev->capture_master_active && castkmsdev->capture_master)
		master = drm_master_get(castkmsdev->capture_master);
	spin_unlock_irqrestore(&castkmsdev->capture_master_lock, flags);

	return master;
}

void castkms_capture_authority_publish_content_owners(
	struct drm_atomic_state *state)
{
	struct castkms_device *castkmsdev =
		drm_device_to_castkms_device(state->dev);
	struct drm_crtc_state *crtc_state;
	struct drm_crtc *crtc;
	unsigned long flags;
	bool changed = false;
	int i;

	for_each_new_crtc_in_state(state, crtc, crtc_state, i) {
		struct castkms_crtc_state *castkms_state =
			to_castkms_crtc_state(crtc_state);
		struct castkms_output *output =
			drm_crtc_to_castkms_output(crtc);
		struct drm_master *new_owner = castkms_state->capture_owner;
		struct drm_master *old_owner;

		if (new_owner)
			drm_master_get(new_owner);
		spin_lock_irqsave(&output->lock, flags);
		old_owner = output->capture_owner;
		output->capture_owner = new_owner;
		output->capture_owner_updating = false;
		output->capture_owner_generation++;
		spin_unlock_irqrestore(&output->lock, flags);
		if (old_owner)
			drm_master_put(&old_owner);
		changed = true;
	}

	if (!changed)
		return;

	spin_lock_irqsave(&castkmsdev->capture_master_lock, flags);
	if (!castkms_capture_authority_device_is_shutdown(castkmsdev)) {
		castkmsdev->capture_master_transition++;
		schedule_work(&castkmsdev->capture_master_work);
	}
	spin_unlock_irqrestore(&castkmsdev->capture_master_lock, flags);
}

void castkms_capture_authority_master_set(
	struct drm_device *dev, struct drm_file *file_priv, bool from_open)
{
	struct castkms_device *castkmsdev = drm_device_to_castkms_device(dev);
	struct drm_master *master = drm_file_get_master(file_priv);
	struct drm_master *old_master = NULL;
	unsigned long flags;

	(void)from_open;
	if (!master)
		return;

	spin_lock_irqsave(&castkmsdev->capture_master_lock, flags);
	if (castkms_capture_authority_device_is_shutdown(castkmsdev)) {
		spin_unlock_irqrestore(&castkmsdev->capture_master_lock, flags);
		drm_master_put(&master);
		return;
	}

	if (castkmsdev->capture_master == master) {
		castkmsdev->capture_master_active = true;
		castkmsdev->capture_master_file = file_priv;
	} else {
		if (castkmsdev->capture_master_active)
			castkmsdev->capture_master_cleanup_sequence++;
		old_master = castkmsdev->capture_master;
		castkmsdev->capture_master = master;
		master = NULL;
		castkmsdev->capture_master_file = file_priv;
		castkmsdev->capture_master_active = true;
	}
	castkmsdev->capture_master_transition++;
	schedule_work(&castkmsdev->capture_master_work);
	spin_unlock_irqrestore(&castkmsdev->capture_master_lock, flags);

	if (old_master)
		drm_master_put(&old_master);
	if (master)
		drm_master_put(&master);
}

void castkms_capture_authority_master_drop(
	struct drm_device *dev, struct drm_file *file_priv)
{
	struct castkms_device *castkmsdev = drm_device_to_castkms_device(dev);
	unsigned long flags;

	spin_lock_irqsave(&castkmsdev->capture_master_lock, flags);
	if (!castkms_capture_authority_device_is_shutdown(castkmsdev) &&
	    castkmsdev->capture_master &&
	    castkmsdev->capture_master_file == file_priv) {
		castkmsdev->capture_master_active = false;
		castkmsdev->capture_master_cleanup_sequence++;
		castkmsdev->capture_master_transition++;
		schedule_work(&castkmsdev->capture_master_work);
	}
	spin_unlock_irqrestore(&castkmsdev->capture_master_lock, flags);
}

void castkms_capture_authority_master_file_close(
	struct drm_device *dev, struct drm_file *file_priv)
{
	struct castkms_device *castkmsdev = drm_device_to_castkms_device(dev);
	unsigned long flags;

	spin_lock_irqsave(&castkmsdev->capture_master_lock, flags);
	if (castkmsdev->capture_master_file == file_priv) {
		bool was_active = castkmsdev->capture_master_active;

		castkmsdev->capture_master_file = NULL;
		castkmsdev->capture_master_active = false;
		if (was_active)
			castkmsdev->capture_master_cleanup_sequence++;
		castkmsdev->capture_master_transition++;
		if (!castkms_capture_authority_device_is_shutdown(castkmsdev))
			schedule_work(&castkmsdev->capture_master_work);
	}
	spin_unlock_irqrestore(&castkmsdev->capture_master_lock, flags);
}

void castkms_capture_authority_revoke_all(
	struct castkms_device *castkmsdev, int status)
{
	struct castkms_capture_authority *authority;
	struct drm_master *master;
	unsigned long flags;
	unsigned long id = 0;

	mutex_lock(&castkmsdev->authority_registry_lock);
	castkms_capture_authority_mark_device_shutdown(castkmsdev);
	mutex_unlock(&castkmsdev->authority_registry_lock);
	cancel_work_sync(&castkmsdev->capture_master_work);

	for (;;) {
		mutex_lock(&castkmsdev->authority_registry_lock);
		authority = xa_find(&castkmsdev->authorities, &id,
				    ULONG_MAX, XA_PRESENT);
		if (authority)
			castkms_capture_authority_get(authority);
		mutex_unlock(&castkmsdev->authority_registry_lock);
		if (!authority)
			break;

		castkms_capture_authority_revoke(authority, status);
		castkms_capture_authority_put(authority);
		/* Revoke removes this entry, so the same cursor finds its successor. */
	}

	spin_lock_irqsave(&castkmsdev->capture_master_lock, flags);
	master = castkmsdev->capture_master;
	castkmsdev->capture_master = NULL;
	castkmsdev->capture_master_file = NULL;
	castkmsdev->capture_master_active = false;
	spin_unlock_irqrestore(&castkmsdev->capture_master_lock, flags);
	if (master)
		drm_master_put(&master);
}

void castkms_capture_authority_stop_connector_streams(
	struct drm_connector *connector,
	struct castkms_capture_authority *skip, int status)
{
	struct castkms_device *castkmsdev =
		drm_device_to_castkms_device(connector->dev);
	struct castkms_capture_authority *authority;
	unsigned long id = 0;

	for (;;) {
		mutex_lock(&castkmsdev->authority_registry_lock);
		authority = xa_find(&castkmsdev->authorities, &id,
				    ULONG_MAX, XA_PRESENT);
		if (authority)
			castkms_capture_authority_get(authority);
		mutex_unlock(&castkmsdev->authority_registry_lock);
		if (!authority)
			break;

		if (authority != skip && authority->connector == connector) {
			mutex_lock(&authority->resource_lock);
			if (!castkms_capture_authority_is_revoked(authority))
				castkms_capture_authority_stop_streams(authority,
								  status);
			mutex_unlock(&authority->resource_lock);
		}
		castkms_capture_authority_put(authority);
		if (id == ULONG_MAX)
			break;
		id++;
	}
}
