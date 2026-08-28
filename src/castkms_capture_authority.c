// SPDX-License-Identifier: GPL-2.0-only

#include <linux/completion.h>
#include <linux/err.h>
#include <linux/kref.h>
#include <linux/limits.h>
#include <linux/slab.h>
#include <linux/xarray.h>

#include <drm/drm_auth.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_managed.h>

#include <kunit/visibility.h>

#include "castkms_capture_authority.h"
#include "castkms_capture_owner.h"
#include "castkms_connector.h"
#include "castkms_device.h"
#include "castkms_output.h"

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
 * @resources: Core resources coupled to authority suspension and revocation
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
	struct list_head resources;
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
	bool master_present, bool master_active, bool bound_master_current,
	bool connector_ready, bool content_safe)
{
	if (permanently_revoked || device_shutdown)
		return CASTKMS_CAPTURE_AUTHORITY_REVOKED;
	if (!master_active || !master_present)
		return CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_NO_MASTER;
	if (!administrative && !bound_master_current)
		return CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_OTHER_MASTER;
	if (!connector_ready)
		return CASTKMS_CAPTURE_AUTHORITY_PENDING;
	if (!content_safe)
		return CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_FOREIGN_CONTENT;

	return CASTKMS_CAPTURE_AUTHORITY_ACTIVE;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_authority_resolve_state);

static void castkms_capture_authority_release(struct kref *ref)
{
	struct castkms_capture_authority *authority =
		container_of(ref, struct castkms_capture_authority, ref);
	const struct castkms_capture_authority_ops *ops = authority->ops;
	void *data = authority->data;

	WARN_ON(!list_empty(&authority->resources));
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
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_authority_put);

int castkms_capture_authority_register_resource(
	struct castkms_capture_authority *authority,
	struct castkms_capture_authority_resource *resource,
	const struct castkms_capture_authority_resource_ops *ops)
{
	int ret;

	if (!resource || !ops || !ops->revoke)
		return -EINVAL;

	mutex_lock(&authority->resource_lock);
	if (resource->authority) {
		ret = -EBUSY;
		goto out_unlock;
	}
	ret = castkms_capture_authority_lifetime_status(authority);
	if (ret)
		goto out_unlock;

	INIT_LIST_HEAD(&resource->link);
	resource->authority = authority;
	resource->ops = ops;
	castkms_capture_authority_get(authority);
	list_add_tail(&resource->link, &authority->resources);

out_unlock:
	mutex_unlock(&authority->resource_lock);
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_authority_register_resource);

bool castkms_capture_authority_unregister_resource(
	struct castkms_capture_authority *authority,
	struct castkms_capture_authority_resource *resource)
{
	bool removed = false;

	mutex_lock(&authority->resource_lock);
	if (resource->authority == authority) {
		list_del_init(&resource->link);
		resource->authority = NULL;
		removed = true;
	}
	mutex_unlock(&authority->resource_lock);

	if (removed)
		castkms_capture_authority_put(authority);
	return removed;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_authority_unregister_resource);

bool castkms_capture_authority_is_revoked(
	const struct castkms_capture_authority *authority)
{
	/* Pairs with revoke's release store before reading @revoke_status. */
	return smp_load_acquire(&authority->revoked);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_authority_is_revoked);

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

bool castkms_capture_authority_is_bound_to_master(
	const struct castkms_capture_authority *authority,
	const struct drm_master *master)
{
	return master && authority->bound_master == master;
}

bool castkms_capture_authority_has_rights(
	const struct castkms_capture_authority *authority, u32 rights)
{
	return (authority->rights & rights) == rights;
}

static int castkms_capture_authority_status(
	const struct castkms_capture_authority *authority)
{
	struct castkms_capture_owner_snapshot ownership;
	int status = 0;

	status = castkms_capture_authority_lifetime_status(authority);
	if (status)
		return status;
	if (authority->administrative)
		return 0;

	castkms_capture_owner_snapshot(&authority->device->drm,
				       authority->bound_master, &ownership);
	if (!ownership.master_active || !ownership.bound_master_current)
		status = -EAGAIN;

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
	struct castkms_capture_owner_snapshot ownership;
	struct castkms_output *output = NULL;
	unsigned long flags;
	bool connector_ready;
	int ret;

	connector_ready = castkms_connector_is_attached(authority->connector);
	if (connector_ready) {
		ret = castkms_connector_get_routed_output(authority->connector,
							  &output);
		if (ret)
			return ret;
	}
	connector_ready = false;

	if (output) {
		spin_lock_irqsave(&output->lock, flags);
		castkms_capture_owner_take_output_snapshot(
			output, authority->bound_master, &ownership);
		connector_ready = output->capture.active;
		spin_unlock_irqrestore(&output->lock, flags);
	} else {
		castkms_capture_owner_snapshot(&castkmsdev->drm,
					       authority->bound_master,
					       &ownership);
	}
	*state = castkms_capture_authority_resolve_state(
		castkms_capture_authority_is_revoked(authority),
		castkms_capture_authority_device_is_shutdown(castkmsdev),
		authority->administrative, ownership.master_present,
		ownership.master_active, ownership.bound_master_current,
		connector_ready, ownership.content_safe);

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

int castkms_capture_authority_evaluate_capture_status(
	const struct castkms_capture_authority *authority,
	const struct castkms_output *output)
{
	struct castkms_capture_owner_snapshot ownership;
	int status;

	lockdep_assert_held(&output->lock);
	status = castkms_capture_authority_lifetime_status(authority);
	if (status)
		return status;
	status = 0;

	castkms_capture_owner_take_output_snapshot(
		output, authority->bound_master, &ownership);
	if (!ownership.master_active || !ownership.master_present)
		status = -EAGAIN;
	else if (!authority->administrative && !ownership.bound_master_current)
		status = -EAGAIN;
	else if (!ownership.content_safe)
		status = -ESTALE;

	return status;
}

int castkms_capture_authority_capture_status(
	const struct castkms_capture_authority *authority,
	struct castkms_output *output)
{
	struct castkms_output *routed_output;
	unsigned long flags;
	int status;

	status = castkms_capture_authority_status(authority);
	if (status)
		return status;
	status = castkms_connector_get_routed_output(
		authority->connector, &routed_output);
	if (status)
		return status;
	if (!routed_output)
		return -ENOLINK;
	if (routed_output != output)
		return -EACCES;

	spin_lock_irqsave(&output->lock, flags);
	status = castkms_capture_authority_evaluate_capture_status(authority,
							      output);
	spin_unlock_irqrestore(&output->lock, flags);

	return status;
}

u64 castkms_capture_authority_stream_generation(
	const struct castkms_capture_authority *authority)
{
	struct castkms_capture_owner_snapshot ownership;

	castkms_capture_owner_snapshot(&authority->device->drm,
				       authority->bound_master, &ownership);

	return ownership.cleanup_sequence;
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

int castkms_capture_authority_check_stream_continuity(
	const struct castkms_capture_authority *authority,
	u64 stream_generation)
{
	struct castkms_capture_owner_snapshot ownership;
	int status;

	status = castkms_capture_authority_lifetime_status(authority);
	if (status)
		return status;
	status = 0;

	castkms_capture_owner_snapshot(&authority->device->drm,
				       authority->bound_master, &ownership);
	if (!castkms_capture_authority_stream_generation_is_current(
		    stream_generation, ownership.cleanup_sequence))
		status = -EAGAIN;
	else if (!authority->administrative &&
		 (!ownership.master_active || !ownership.bound_master_current))
		status = -EAGAIN;

	return status;
}

int castkms_capture_authority_evaluate_stream_status(
	const struct castkms_capture_authority *authority,
	const struct castkms_output *output, u64 stream_generation)
{
	struct castkms_capture_owner_snapshot ownership;
	int status;

	lockdep_assert_held(&output->lock);
	status = castkms_capture_authority_lifetime_status(authority);
	if (status)
		return status;
	status = 0;

	castkms_capture_owner_take_output_snapshot(
		output, authority->bound_master, &ownership);
	if (!castkms_capture_authority_stream_generation_is_current(
		    stream_generation, ownership.cleanup_sequence))
		status = -EAGAIN;
	else if (!ownership.master_active || !ownership.master_present)
		status = -EAGAIN;
	else if (!authority->administrative && !ownership.bound_master_current)
		status = -EAGAIN;
	else if (!ownership.content_safe)
		status = -ESTALE;

	return status;
}

int castkms_capture_authority_stream_status(
	const struct castkms_capture_authority *authority,
	struct castkms_output *output, u64 stream_generation)
{
	struct castkms_output *routed_output;
	unsigned long flags;
	int status;

	status = castkms_connector_get_routed_output(
		authority->connector, &routed_output);
	if (status)
		return status;
	if (!routed_output)
		return -ENOLINK;
	if (routed_output != output)
		return -EACCES;

	spin_lock_irqsave(&output->lock, flags);
	status = castkms_capture_authority_evaluate_stream_status(
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
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_authority_begin);

int castkms_capture_authority_begin_output(
	struct castkms_capture_authority *authority,
	struct castkms_output *output, u32 rights)
{
	int status;

	if (!authority)
		return -EACCES;
	status = castkms_capture_authority_lifetime_status(authority);
	if (status)
		return status;
	if (!castkms_capture_authority_has_rights(authority, rights))
		return -EACCES;

	status = castkms_capture_authority_begin(authority, NULL, rights);
	if (status)
		return status;
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
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_authority_end);

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
	struct castkms_capture_owner_snapshot ownership;
	int ret;

	if (!rights || rights & ~CASTKMS_CAPTURE_AUTHORITY_RIGHTS_MASK)
		return ERR_PTR(-EINVAL);
	if (!connector ||
	    connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK)
		return ERR_PTR(-ENOENT);
	if (!administrative && !bound_master)
		return ERR_PTR(-EACCES);

	if (castkms_capture_authority_device_is_shutdown(castkmsdev))
		return ERR_PTR(-ENODEV);

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
	INIT_LIST_HEAD(&authority->resources);
	init_completion(&authority->cleanup_done);

	castkms_capture_owner_snapshot(&castkmsdev->drm, bound_master,
				       &ownership);
	authority->master_cleanup_handled = ownership.cleanup_sequence;

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
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_authority_create);

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

static void castkms_capture_authority_revoke_resource(
	struct castkms_capture_authority *authority,
	struct castkms_capture_authority_resource *resource, int status)
{
	const struct castkms_capture_authority_resource_ops *ops = resource->ops;

	lockdep_assert_held(&authority->resource_lock);
	list_del_init(&resource->link);
	resource->authority = NULL;
	ops->revoke(resource, status);
	castkms_capture_authority_put(authority);
}

static void castkms_capture_authority_revoke_resources(
	struct castkms_capture_authority *authority, int status)
{
	struct castkms_capture_authority_resource *resource, *next;

	lockdep_assert_held(&authority->resource_lock);
	list_for_each_entry_safe(resource, next, &authority->resources, link)
		castkms_capture_authority_revoke_resource(
			authority, resource, status);
}

static void castkms_capture_authority_cleanup_resources(
	struct castkms_capture_authority *authority,
	enum castkms_capture_authority_cleanup_reason reason,
	u64 generation, int status)
{
	struct castkms_capture_authority_resource *resource, *next;

	lockdep_assert_held(&authority->resource_lock);
	list_for_each_entry_safe(resource, next, &authority->resources, link) {
		if (!resource->ops->needs_cleanup ||
		    !resource->ops->needs_cleanup(resource, reason, generation))
			continue;
		castkms_capture_authority_revoke_resource(
			authority, resource, status);
	}
}

static void castkms_capture_authority_suspend_resources(
	struct castkms_capture_authority *authority, int status)
{
	struct castkms_capture_authority_resource *resource;

	lockdep_assert_held(&authority->resource_lock);
	list_for_each_entry(resource, &authority->resources, link) {
		if (resource->ops->suspend)
			resource->ops->suspend(resource, status);
	}
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
	castkms_capture_authority_revoke_resources(authority, status);
	detached = castkms_connector_detach_authority(authority);
	mutex_unlock(&authority->resource_lock);

	/* Never acquire a foreign authority lock while holding this one. */
	if (detached)
		castkms_capture_authority_cleanup_connector_resources(
			authority->connector, authority, -ENOTCONN);
	if (authority->ops && authority->ops->revoked)
		authority->ops->revoked(authority, status, authority->data);
	complete_all(&authority->cleanup_done);
	castkms_capture_authority_unregister(authority);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_authority_revoke);

static void castkms_capture_authority_reconcile(
	struct castkms_capture_authority *authority,
	u64 master_cleanup_sequence)
{
	enum castkms_capture_authority_state state;
	bool force_master_cleanup;
	int status;

	mutex_lock(&authority->lock);
	mutex_lock(&authority->resource_lock);
	if (castkms_capture_authority_get_state(authority, &state)) {
		mutex_unlock(&authority->resource_lock);
		mutex_unlock(&authority->lock);
		return;
	}
	if (state == CASTKMS_CAPTURE_AUTHORITY_REVOKED) {
		mutex_unlock(&authority->resource_lock);
		mutex_unlock(&authority->lock);
		return;
	}

	force_master_cleanup =
		authority->master_cleanup_handled < master_cleanup_sequence;
	if (force_master_cleanup)
		authority->master_cleanup_handled = master_cleanup_sequence;

	status = castkms_capture_authority_state_status(authority, state);
	if (force_master_cleanup)
		castkms_capture_authority_cleanup_resources(
			authority,
			CASTKMS_CAPTURE_AUTHORITY_CLEANUP_MASTER_EPOCH,
			master_cleanup_sequence, -EAGAIN);
	if (!authority->administrative &&
	    (force_master_cleanup ||
	     state == CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_NO_MASTER ||
	     state == CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_OTHER_MASTER))
		castkms_capture_authority_suspend_resources(authority, status);
	if (authority->ops && authority->ops->state_changed)
		authority->ops->state_changed(authority, state, status,
						      authority->data);
	mutex_unlock(&authority->resource_lock);
	mutex_unlock(&authority->lock);
}

static void castkms_capture_authority_reconcile_ownership(
	struct castkms_device *castkmsdev, u64 cleanup_sequence)
{
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

		castkms_capture_authority_reconcile(authority,
						 cleanup_sequence);
		castkms_capture_authority_put(authority);
		if (id == ULONG_MAX)
			break;
		id++;
	}
}

static void castkms_capture_authority_ownership_changed(
	void *data, u64 cleanup_sequence)
{
	struct castkms_device *castkmsdev = data;

	castkms_capture_authority_reconcile_ownership(castkmsdev,
						     cleanup_sequence);
}

static const struct castkms_capture_owner_ops
castkms_capture_authority_owner_ops = {
	.changed = castkms_capture_authority_ownership_changed,
};

static void castkms_capture_authority_device_fini(
	struct drm_device *dev, void *data)
{
	struct castkms_device *castkmsdev = data;

	(void)dev;
	castkms_capture_authority_mark_device_shutdown(castkmsdev);
	castkms_capture_owner_device_fini(castkmsdev);

	WARN_ON(!xa_empty(&castkmsdev->authorities));
	xa_destroy(&castkmsdev->authorities);
	mutex_destroy(&castkmsdev->authority_registry_lock);
}

int castkms_capture_authority_device_init(struct castkms_device *castkmsdev)
{
	mutex_init(&castkmsdev->authority_registry_lock);
	xa_init_flags(&castkmsdev->authorities, XA_FLAGS_ALLOC);
	castkmsdev->next_authority_id = 1;
	castkms_capture_owner_device_init(
		castkmsdev, &castkms_capture_authority_owner_ops, castkmsdev);

	return drmm_add_action_or_reset(&castkmsdev->drm,
					castkms_capture_authority_device_fini,
					castkmsdev);
}

void castkms_capture_authority_revoke_all(
	struct castkms_device *castkmsdev, int status)
{
	struct castkms_capture_authority *authority;
	unsigned long id = 0;

	mutex_lock(&castkmsdev->authority_registry_lock);
	castkms_capture_authority_mark_device_shutdown(castkmsdev);
	mutex_unlock(&castkmsdev->authority_registry_lock);
	castkms_capture_owner_device_fini(castkmsdev);

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
}

void castkms_capture_authority_cleanup_connector_resources(
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
			mutex_lock(&authority->lock);
			mutex_lock(&authority->resource_lock);
			if (!castkms_capture_authority_is_revoked(authority))
				castkms_capture_authority_cleanup_resources(
					authority,
					CASTKMS_CAPTURE_AUTHORITY_CLEANUP_DISCONNECT,
					0, status);
			mutex_unlock(&authority->resource_lock);
			mutex_unlock(&authority->lock);
		}
		castkms_capture_authority_put(authority);
		if (id == ULONG_MAX)
			break;
		id++;
	}
}
