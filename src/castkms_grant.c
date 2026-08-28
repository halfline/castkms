// SPDX-License-Identifier: GPL-2.0-only

#include <linux/anon_inodes.h>
#include <linux/build_bug.h>
#include <linux/capability.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/ktime.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/user_namespace.h>
#include <linux/xarray.h>

#include <drm/castkms_drm.h>
#include <drm/drm_auth.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>

#include <kunit/visibility.h>

#include "castkms_capture_authority.h"
#include "castkms_file.h"
#include "castkms_grant.h"
#include "castkms_grant_file.h"
#include "castkms_output.h"
#include "castkms_uapi_device.h"

/**
 * struct castkms_grant_registry - Device-local grant-fd UAPI namespace
 * @lock: Serializes IDs and revoker associations
 * @grants: Live grant wrappers retained through holder final close
 * @next_id: Cyclic allocation cursor for device-unique UAPI grant IDs
 *
 * This state is private to the grant adapter. The outer UAPI device shell
 * retains an opaque pointer; the core device, authority, and CEC interfaces
 * cannot observe the registry or its grant IDs.
 */
struct castkms_grant_registry {
	struct mutex lock;
	struct xarray grants;
	u32 next_id;
};

static struct castkms_grant_registry *
castkms_grant_registry(struct drm_device *dev)
{
	return drm_device_to_castkms_uapi_device(dev)->grant_registry;
}

/**
 * struct castkms_capture_grant - grant-fd UAPI wrapper
 * @authority: Kernel-native authorization and security state
 * @dev: Owning DRM device, pinned separately by each live grantor file
 * @lock: Protects UAPI file associations, event state, and reported state
 * @id: Device-unique UAPI grant ID
 * @reported_state: Latest state reported through the grant fd
 * @published: The UAPI ID and holder event stream are ready for use
 * @revoker_file: Optional close-to-revoke file state, protected by the
 * grant-registry lock
 * @holder_file: Fresh DRM file carrying this grant, valid through cleanup
 * @revoke_event: Pre-reserved terminal event
 * @delegated: Grant is holder-lived and bound to a master selected by an
 * administrator
 *
 * Capture streams, connector attachments, and CEC transports retain only
 * @authority.  This wrapper translates file descriptors, IDs, and DRM events
 * without becoming part of the kernel-native capture mechanism.
 */
struct castkms_capture_grant {
	struct castkms_capture_authority *authority;
	struct drm_device *dev;
	struct mutex lock; /* Protects UAPI associations and event state. */
	u32 id;
	enum castkms_capture_authority_state reported_state;
	bool published;
	bool delegated;
	struct castkms_file *revoker_file;
	struct drm_file *holder_file;
	struct castkms_grant_pending_event *revoke_event;
};

struct castkms_grant_pending_event {
	struct drm_pending_event pending;
	struct drm_event_castkms_grant_revoked event;
};

struct castkms_grant_state_pending_event {
	struct drm_pending_event pending;
	struct drm_event_castkms_grant_state event;
};

static_assert(sizeof(struct drm_castkms_create_grant) == 32);
static_assert(offsetof(struct drm_castkms_create_grant, connector_id) == 0);
static_assert(offsetof(struct drm_castkms_create_grant, rights) == 4);
static_assert(offsetof(struct drm_castkms_create_grant, flags) == 8);
static_assert(offsetof(struct drm_castkms_create_grant, fd) == 12);
static_assert(offsetof(struct drm_castkms_create_grant, grant_id) == 16);
static_assert(offsetof(struct drm_castkms_create_grant, fd_flags) == 20);
static_assert(offsetof(struct drm_castkms_create_grant, control_fd) == 24);
static_assert(offsetof(struct drm_castkms_create_grant, reserved) == 28);
static_assert(_IOC_SIZE(DRM_IOCTL_CASTKMS_CREATE_GRANT) == 32);
static_assert(sizeof(struct drm_castkms_revoke_grant) == 16);
static_assert(sizeof(struct drm_castkms_get_grant) == 32);
static_assert(sizeof(struct drm_event_castkms_grant_revoked) == 24);
static_assert(sizeof(struct drm_event_castkms_grant_state) == 32);

static int castkms_grant_control_release(struct inode *inode,
					 struct file *file)
{
	struct castkms_capture_grant *grant = file->private_data;
	struct castkms_capture_authority *authority = grant->authority;
	struct drm_device *dev = grant->dev;

	file->private_data = NULL;
	castkms_capture_authority_revoke(authority, -EKEYREVOKED);
	castkms_capture_authority_put(authority);
	drm_dev_put(dev);

	return 0;
}

static const struct file_operations castkms_grant_control_fops = {
	.owner = THIS_MODULE,
	.release = castkms_grant_control_release,
	.llseek = noop_llseek,
};

static struct file *castkms_grant_control_file_create(
	struct castkms_capture_grant *grant)
{
	struct file *file;

	drm_dev_get(grant->dev);
	castkms_capture_authority_get(grant->authority);
	file = anon_inode_getfile("castkms-grant-control",
				  &castkms_grant_control_fops, grant, O_RDONLY);
	if (IS_ERR(file)) {
		castkms_capture_authority_put(grant->authority);
		drm_dev_put(grant->dev);
	}

	return file;
}

static u32 castkms_grant_state_to_uapi(
	enum castkms_capture_authority_state state)
{
	switch (state) {
	case CASTKMS_CAPTURE_AUTHORITY_PENDING:
		return DRM_CASTKMS_GRANT_STATE_PENDING;
	case CASTKMS_CAPTURE_AUTHORITY_ACTIVE:
		return DRM_CASTKMS_GRANT_STATE_ACTIVE;
	case CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_NO_MASTER:
		return DRM_CASTKMS_GRANT_STATE_SUSPENDED_NO_MASTER;
	case CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_OTHER_MASTER:
		return DRM_CASTKMS_GRANT_STATE_SUSPENDED_OTHER_MASTER;
	case CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_FOREIGN_CONTENT:
		return DRM_CASTKMS_GRANT_STATE_SUSPENDED_FOREIGN_CONTENT;
	case CASTKMS_CAPTURE_AUTHORITY_REVOKED:
	default:
		return DRM_CASTKMS_GRANT_STATE_REVOKED;
	}
}

static u32 castkms_grant_rights_from_uapi(u32 rights)
{
	u32 authority_rights = 0;

	if (rights & DRM_CASTKMS_GRANT_CAPTURE_PIXELS)
		authority_rights |= CASTKMS_CAPTURE_AUTHORITY_CAPTURE_PIXELS;
	if (rights & DRM_CASTKMS_GRANT_MANAGE_ATTACHMENT)
		authority_rights |= CASTKMS_CAPTURE_AUTHORITY_MANAGE_ATTACHMENT;
	if (rights & DRM_CASTKMS_GRANT_UPDATE_EDID)
		authority_rights |= CASTKMS_CAPTURE_AUTHORITY_UPDATE_EDID;
	if (rights & DRM_CASTKMS_GRANT_READ_CURSOR)
		authority_rights |= CASTKMS_CAPTURE_AUTHORITY_READ_CURSOR;
	if (rights & DRM_CASTKMS_GRANT_MANAGE_CEC)
		authority_rights |= CASTKMS_CAPTURE_AUTHORITY_MANAGE_CEC;

	return authority_rights;
}

static u32 castkms_grant_rights_to_uapi(u32 rights)
{
	u32 uapi_rights = 0;

	if (rights & CASTKMS_CAPTURE_AUTHORITY_CAPTURE_PIXELS)
		uapi_rights |= DRM_CASTKMS_GRANT_CAPTURE_PIXELS;
	if (rights & CASTKMS_CAPTURE_AUTHORITY_MANAGE_ATTACHMENT)
		uapi_rights |= DRM_CASTKMS_GRANT_MANAGE_ATTACHMENT;
	if (rights & CASTKMS_CAPTURE_AUTHORITY_UPDATE_EDID)
		uapi_rights |= DRM_CASTKMS_GRANT_UPDATE_EDID;
	if (rights & CASTKMS_CAPTURE_AUTHORITY_READ_CURSOR)
		uapi_rights |= DRM_CASTKMS_GRANT_READ_CURSOR;
	if (rights & CASTKMS_CAPTURE_AUTHORITY_MANAGE_CEC)
		uapi_rights |= DRM_CASTKMS_GRANT_MANAGE_CEC;

	return uapi_rights;
}

static struct castkms_capture_grant *
castkms_grant_from_file(struct drm_file *file_priv)
{
	struct castkms_file *file_state = file_priv->driver_priv;

	return file_state ? READ_ONCE(file_state->holder_grant) : NULL;
}

VISIBLE_IF_KUNIT bool castkms_grant_id_access_allowed(
	bool owns_revoker, bool privileged, bool delegated,
	bool caller_is_bound_owner)
{
	return owns_revoker || privileged ||
		(delegated && caller_is_bound_owner);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_grant_id_access_allowed);

static struct drm_master *castkms_grant_caller_owner_master_get(
	struct drm_device *dev, struct drm_file *file_priv)
{
	struct drm_master *master = NULL;

	mutex_lock(&dev->master_mutex);
	if (drm_is_current_master(file_priv) && file_priv->master &&
	    castkms_grant_master_is_owner(file_priv->master))
		master = drm_master_get(file_priv->master);
	mutex_unlock(&dev->master_mutex);

	return master;
}

static struct castkms_capture_grant *castkms_grant_lookup_by_id(
	struct drm_device *dev, struct castkms_file *file_state,
	struct drm_file *file_priv, u32 grant_id)
{
	struct castkms_grant_registry *registry = castkms_grant_registry(dev);
	struct drm_master *caller_master;
	struct castkms_capture_grant *grant;
	bool privileged;
	bool owns_revoker;
	bool bound_owner;

	privileged = ns_capable(&init_user_ns, CAP_SYS_ADMIN);
	caller_master = castkms_grant_caller_owner_master_get(dev, file_priv);

	mutex_lock(&registry->lock);
	grant = xa_load(&file_state->revocable_grants, grant_id);
	owns_revoker = !!grant;
	if (!grant)
		grant = xa_load(&registry->grants, grant_id);
	bound_owner = grant && caller_master &&
		castkms_capture_authority_is_bound_to_master(
			grant->authority, caller_master);
	if (grant && !castkms_grant_id_access_allowed(
			     owns_revoker, privileged, grant->delegated,
			     bound_owner))
		grant = NULL;
	if (grant)
		castkms_capture_authority_get(grant->authority);
	mutex_unlock(&registry->lock);

	if (caller_master)
		drm_master_put(&caller_master);
	return grant;
}

static void castkms_grant_state_changed(
	struct castkms_capture_authority *authority,
	enum castkms_capture_authority_state state, int status, void *data)
{
	struct castkms_capture_grant *grant = data;
	struct castkms_grant_state_pending_event *pending;
	struct drm_file *holder_file;
	struct file *active_file = NULL;
	int ret;

	mutex_lock(&grant->lock);
	if (castkms_capture_authority_is_revoked(authority) ||
	    grant->reported_state == state) {
		mutex_unlock(&grant->lock);
		return;
	}
	grant->reported_state = state;
	if (!grant->published)
		goto out_unlock;
	holder_file = grant->holder_file;
	if (holder_file)
		active_file = get_file_active(&holder_file->filp);
	if (!active_file)
		goto out_unlock;

	pending = kzalloc_obj(*pending);
	if (!pending)
		goto out_unlock;
	pending->event.base.type = DRM_CASTKMS_CAPTURE_EVENT_GRANT_STATE;
	pending->event.base.length = sizeof(pending->event);
	pending->event.grant_id = grant->id;
	pending->event.state = castkms_grant_state_to_uapi(state);
	pending->event.status = status;
	pending->event.timestamp_ns = ktime_get_ns();
	ret = drm_event_reserve_init(holder_file->minor->dev, holder_file,
				     &pending->pending,
				     &pending->event.base);
	if (ret) {
		kfree(pending);
		goto out_unlock;
	}

	drm_send_event(holder_file->minor->dev, &pending->pending);

out_unlock:
	mutex_unlock(&grant->lock);
	if (active_file)
		fput(active_file);
}

static void castkms_grant_revoked(
	struct castkms_capture_authority *authority, int status, void *data)
{
	struct castkms_capture_grant *grant = data;
	struct castkms_grant_pending_event *pending;
	struct drm_device *dev = grant->dev;

	mutex_lock(&grant->lock);
	pending = grant->revoke_event;
	grant->revoke_event = NULL;
	if (pending) {
		pending->event.grant_id = grant->id;
		pending->event.status = status;
		pending->event.timestamp_ns = ktime_get_ns();
	}
	mutex_unlock(&grant->lock);

	if (pending)
		drm_send_event(dev, &pending->pending);
}

static void castkms_grant_release(
	struct castkms_capture_authority *authority, void *data)
{
	struct castkms_capture_grant *grant = data;

	WARN_ON(grant->holder_file);
	WARN_ON(grant->revoke_event);
	mutex_destroy(&grant->lock);
	kfree(grant);
}

static const struct castkms_capture_authority_ops castkms_grant_authority_ops = {
	.state_changed = castkms_grant_state_changed,
	.revoked = castkms_grant_revoked,
	.release = castkms_grant_release,
};

VISIBLE_IF_KUNIT bool
castkms_grant_master_is_owner(const struct drm_master *master)
{
	return master && !master->lessor;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_grant_master_is_owner);

VISIBLE_IF_KUNIT int castkms_grant_creation_status(
	u32 flags, bool privileged, bool caller_current_master,
	bool caller_owner_master, bool current_owner_master)
{
	bool administrative = flags & DRM_CASTKMS_GRANT_CREATE_ADMIN;
	bool delegated = flags & DRM_CASTKMS_GRANT_CREATE_DELEGATED;

	if (flags & ~DRM_CASTKMS_GRANT_CREATE_FLAGS_MASK)
		return -EINVAL;
	if (administrative && delegated)
		return -EINVAL;
	if (administrative)
		return privileged ? 0 : -EACCES;
	if (delegated) {
		if (!privileged)
			return -EACCES;
		if (caller_current_master || !current_owner_master)
			return -EAGAIN;
		return 0;
	}

	return caller_owner_master ? 0 : -EACCES;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_grant_creation_status);

static int castkms_grant_select_master(
	struct drm_device *dev, struct drm_file *file_priv, u32 flags,
	bool privileged, struct drm_master **bound_master_out)
{
	struct drm_master *bound_master = NULL;
	bool caller_current_master;
	bool caller_owner_master;
	bool current_owner_master;
	int ret;

	*bound_master_out = NULL;
	mutex_lock(&dev->master_mutex);
	caller_current_master = drm_is_current_master(file_priv);
	caller_owner_master = caller_current_master && file_priv->master &&
		castkms_grant_master_is_owner(file_priv->master) &&
		dev->master == file_priv->master;
	current_owner_master =
		castkms_grant_master_is_owner(dev->master);
	ret = castkms_grant_creation_status(
		flags, privileged, caller_current_master, caller_owner_master,
		current_owner_master);
	if (!ret && !(flags & DRM_CASTKMS_GRANT_CREATE_ADMIN)) {
		if (flags & DRM_CASTKMS_GRANT_CREATE_DELEGATED)
			bound_master = drm_master_get(dev->master);
		else
			bound_master = drm_master_get(file_priv->master);
	}
	mutex_unlock(&dev->master_mutex);

	*bound_master_out = bound_master;
	return ret;
}

int castkms_grant_begin(struct drm_file *file_priv,
			struct drm_connector *connector, u32 rights,
			struct castkms_capture_authority **authority_out)
{
	struct castkms_capture_grant *grant = castkms_grant_from_file(file_priv);
	struct castkms_capture_authority *authority;
	int ret;

	*authority_out = NULL;
	if (!grant)
		return -EACCES;
	authority = grant->authority;
	ret = castkms_capture_authority_begin(authority, connector, rights);
	if (ret)
		return ret;

	*authority_out = authority;
	return 0;
}

int castkms_grant_begin_crtc(
	struct drm_file *file_priv, struct drm_crtc *crtc, u32 rights,
	struct castkms_capture_authority **authority_out)
{
	struct castkms_capture_grant *grant = castkms_grant_from_file(file_priv);
	struct castkms_capture_authority *authority;
	int ret;

	*authority_out = NULL;
	if (!grant)
		return -EACCES;
	authority = grant->authority;
	ret = castkms_capture_authority_begin_output(
		authority, drm_crtc_to_castkms_output(crtc), rights);
	if (ret)
		return ret;

	*authority_out = authority;
	return 0;
}

int castkms_grant_begin_owned(
	struct drm_file *file_priv,
	struct castkms_capture_authority *owned_authority, u32 rights,
	struct castkms_capture_authority **authority_out)
{
	struct castkms_capture_authority *authority;
	int ret;

	ret = castkms_grant_begin(file_priv, NULL, rights, &authority);
	if (ret)
		return ret;
	if (authority != owned_authority) {
		castkms_capture_authority_end(authority);
		return -EACCES;
	}

	*authority_out = authority;
	return 0;
}

void castkms_grant_end(struct castkms_capture_authority *authority)
{
	castkms_capture_authority_end(authority);
}

static int castkms_grant_register(
	struct castkms_grant_registry *registry,
	struct castkms_capture_grant *grant,
	struct castkms_file *revoker_file)
{
	void *old;
	int ret;

	mutex_lock(&registry->lock);
	mutex_lock(&grant->lock);
	ret = castkms_capture_authority_lifetime_status(grant->authority);
	if (ret)
		goto out_unlock;

	castkms_capture_authority_get(grant->authority);
	ret = xa_alloc_cyclic(&registry->grants, &grant->id, grant,
			      XA_LIMIT(1, INT_MAX),
			      &registry->next_id, GFP_KERNEL);
	/* xa_alloc_cyclic() returns 1 when allocation succeeds after wrapping. */
	if (ret < 0) {
		castkms_capture_authority_put(grant->authority);
		goto out_unlock;
	}

	if (revoker_file) {
		old = xa_store(&revoker_file->revocable_grants, grant->id,
			       grant, GFP_KERNEL);
		ret = xa_err(old);
		if (ret) {
			xa_erase(&registry->grants, grant->id);
			castkms_capture_authority_put(grant->authority);
			grant->id = 0;
			goto out_unlock;
		}
	}
	grant->revoker_file = revoker_file;
	grant->published = true;

out_unlock:
	mutex_unlock(&grant->lock);
	mutex_unlock(&registry->lock);
	return ret;
}

int castkms_grant_create_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file_priv)
{
	struct drm_castkms_create_grant *args = data;
	struct castkms_device *core_device = drm_device_to_castkms_device(dev);
	struct castkms_grant_registry *registry = castkms_grant_registry(dev);
	struct castkms_file *creator_file = file_priv->driver_priv;
	struct castkms_file *revoker_file;
	struct castkms_file *holder_file_state = NULL;
	struct castkms_grant_pending_event *pending = NULL;
	struct castkms_capture_grant *grant;
	struct castkms_capture_authority *authority;
	struct drm_connector *connector;
	struct drm_master *bound_master = NULL;
	struct drm_file *holder_file;
	struct file *control_file = NULL;
	struct file *grant_file = NULL;
	bool holder_ref = false;
	bool administrative;
	bool delegated;
	bool privileged;
	int control_fd = -1;
	int fd = -1;
	int ret;

	if (args->control_fd != -1 || args->reserved)
		return -EINVAL;
	args->fd = -1;
	args->control_fd = -1;
	args->grant_id = 0;
	if (!args->rights || (args->rights & ~DRM_CASTKMS_GRANT_RIGHTS_MASK) ||
	    (args->flags & ~DRM_CASTKMS_GRANT_CREATE_FLAGS_MASK) ||
	    (args->fd_flags & ~O_NONBLOCK))
		return -EINVAL;
	if (!creator_file || creator_file->holder_grant)
		return -EACCES;
	if (!drm_is_primary_client(file_priv))
		return -EACCES;

	privileged = ns_capable(&init_user_ns, CAP_SYS_ADMIN);
	ret = castkms_grant_select_master(dev, file_priv, args->flags,
					 privileged, &bound_master);
	if (ret)
		return ret;
	administrative = args->flags & DRM_CASTKMS_GRANT_CREATE_ADMIN;
	delegated = args->flags & DRM_CASTKMS_GRANT_CREATE_DELEGATED;
	revoker_file = delegated ? NULL : creator_file;

	connector = drm_connector_lookup(dev, file_priv, args->connector_id);
	if (!connector) {
		ret = -ENOENT;
		goto out_put_master;
	}
	if (connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
		ret = -ENOENT;
		goto out_put_connector;
	}

	grant = kzalloc_obj(*grant);
	if (!grant) {
		ret = -ENOMEM;
		goto out_put_connector;
	}
	mutex_init(&grant->lock);
	grant->dev = dev;
	grant->delegated = delegated;
	authority = castkms_capture_authority_create(
		core_device, connector, bound_master,
		castkms_grant_rights_from_uapi(args->rights),
		administrative, &castkms_grant_authority_ops, grant);
	if (IS_ERR(authority)) {
		ret = PTR_ERR(authority);
		mutex_destroy(&grant->lock);
		kfree(grant);
		goto out_put_connector;
	}
	grant->authority = authority;
	mutex_lock(&grant->lock);
	ret = castkms_capture_authority_get_state(authority,
						 &grant->reported_state);
	mutex_unlock(&grant->lock);
	if (ret)
		goto out_revoke_authority;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		ret = fd;
		goto out_revoke_authority;
	}
	control_fd = get_unused_fd_flags(O_CLOEXEC);
	if (control_fd < 0) {
		ret = control_fd;
		goto out_put_fd;
	}

	grant_file = castkms_grant_file_create(dev, args->fd_flags);
	if (IS_ERR(grant_file)) {
		ret = PTR_ERR(grant_file);
		grant_file = NULL;
		goto out_put_control_fd;
	}
	holder_file = grant_file->private_data;
	grant_file->f_flags &= ~O_NONBLOCK;
	grant_file->f_flags |= args->fd_flags & O_NONBLOCK;

	pending = kzalloc_obj(*pending);
	if (!pending) {
		ret = -ENOMEM;
		goto out_fput;
	}
	pending->event.base.type = DRM_CASTKMS_CAPTURE_EVENT_GRANT_REVOKED;
	pending->event.base.length = sizeof(pending->event);
	ret = drm_event_reserve_init(dev, holder_file, &pending->pending,
				     &pending->event.base);
	if (ret)
		goto out_free_pending;

	holder_file_state = holder_file->driver_priv;
	if (WARN_ON(!holder_file_state)) {
		ret = -EINVAL;
		goto out_cancel_event;
	}
	mutex_lock(&grant->lock);
	grant->holder_file = holder_file;
	grant->revoke_event = pending;
	mutex_unlock(&grant->lock);
	pending = NULL;
	castkms_capture_authority_get(authority);
	holder_ref = true;
	holder_file_state->holder_grant = grant;

	control_file = castkms_grant_control_file_create(grant);
	if (IS_ERR(control_file)) {
		ret = PTR_ERR(control_file);
		control_file = NULL;
		goto out_clear_holder;
	}

	ret = castkms_grant_register(registry, grant, revoker_file);
	if (ret)
		goto out_clear_holder;

	args->fd = fd;
	args->control_fd = control_fd;
	args->grant_id = grant->id;
	fd_install(fd, grant_file);
	fd_install(control_fd, control_file);
	drm_connector_put(connector);
	if (bound_master)
		drm_master_put(&bound_master);
	castkms_capture_authority_put(authority);
	return 0;

out_clear_holder:
	holder_file_state->holder_grant = NULL;
	mutex_lock(&grant->lock);
	grant->holder_file = NULL;
	pending = grant->revoke_event;
	grant->revoke_event = NULL;
	mutex_unlock(&grant->lock);
	if (holder_ref)
		castkms_capture_authority_put(authority);
out_cancel_event:
	if (pending) {
		drm_event_cancel_free(dev, &pending->pending);
		pending = NULL;
	}
out_free_pending:
	kfree(pending);
out_fput:
	fput(grant_file);
	if (control_file)
		fput(control_file);
out_put_control_fd:
	put_unused_fd(control_fd);
out_put_fd:
	put_unused_fd(fd);
out_revoke_authority:
	castkms_capture_authority_revoke(authority, -EKEYREVOKED);
	castkms_capture_authority_put(authority);
out_put_connector:
	drm_connector_put(connector);
out_put_master:
	if (bound_master)
		drm_master_put(&bound_master);
	return ret;
}

int castkms_grant_revoke_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file_priv)
{
	struct drm_castkms_revoke_grant *args = data;
	struct castkms_file *revoker_file = file_priv->driver_priv;
	struct castkms_capture_grant *grant;

	if (args->flags || args->reserved)
		return -EINVAL;
	if (!revoker_file || revoker_file->holder_grant)
		return -EACCES;

	grant = castkms_grant_lookup_by_id(dev, revoker_file, file_priv,
					  args->grant_id);
	if (!grant)
		return -ENOENT;

	castkms_capture_authority_revoke(grant->authority, -EKEYREVOKED);
	castkms_capture_authority_put(grant->authority);
	return 0;
}

int castkms_grant_get_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file_priv)
{
	struct drm_castkms_get_grant *args = data;
	struct castkms_file *file_state = file_priv->driver_priv;
	struct castkms_capture_authority *authority;
	struct castkms_capture_grant *grant;
	enum castkms_capture_authority_state state;
	int ret;

	if (args->flags || args->reserved || args->reserved2)
		return -EINVAL;
	if (!file_state)
		return -ENODATA;

	grant = castkms_grant_from_file(file_priv);
	if (grant) {
		if (args->grant_id && args->grant_id != grant->id)
			return -ENOENT;
		castkms_capture_authority_get(grant->authority);
	} else if (args->grant_id) {
		grant = castkms_grant_lookup_by_id(dev, file_state, file_priv,
						  args->grant_id);
	} else {
		grant = NULL;
	}
	if (!grant)
		return -ENODATA;

	authority = grant->authority;
	ret = castkms_capture_authority_get_state(authority, &state);
	if (ret) {
		castkms_capture_authority_put(authority);
		return ret;
	}
	mutex_lock(&grant->lock);
	args->grant_id = grant->id;
	args->connector_id =
		castkms_capture_authority_connector(authority)->base.id;
	args->rights = castkms_grant_rights_to_uapi(
		castkms_capture_authority_rights(authority));
	if (castkms_capture_authority_is_revoked(authority))
		state = CASTKMS_CAPTURE_AUTHORITY_REVOKED;
	args->state = castkms_grant_state_to_uapi(state);
	args->flags = 0;
	if (castkms_capture_authority_is_administrative(authority))
		args->flags |= DRM_CASTKMS_GRANT_FLAG_ADMIN;
	if (grant->delegated)
		args->flags |= DRM_CASTKMS_GRANT_FLAG_DELEGATED;
	args->reserved = 0;
	args->reserved2 = 0;
	mutex_unlock(&grant->lock);
	castkms_capture_authority_put(authority);

	return 0;
}

static void castkms_grant_close_revoker(
	struct castkms_grant_registry *registry,
	struct castkms_file *revoker_file)
{
	struct castkms_capture_grant *grant;
	unsigned long id;

	for (;;) {
		id = 0;
		mutex_lock(&registry->lock);
		grant = xa_find(&revoker_file->revocable_grants, &id, ULONG_MAX,
				XA_PRESENT);
		if (grant) {
			xa_erase(&revoker_file->revocable_grants, id);
			if (grant->revoker_file == revoker_file)
				grant->revoker_file = NULL;
			castkms_capture_authority_get(grant->authority);
		}
		mutex_unlock(&registry->lock);
		if (!grant)
			break;

		castkms_capture_authority_revoke(grant->authority,
						 -EKEYREVOKED);
		castkms_capture_authority_put(grant->authority);
	}
}

void castkms_grant_uapi_file_fini(struct drm_device *dev,
				  struct drm_file *file_priv)
{
	struct castkms_grant_registry *registry = castkms_grant_registry(dev);
	struct castkms_file *file_state = file_priv->driver_priv;
	struct castkms_capture_grant *grant;
	struct castkms_capture_authority *authority;
	bool put_registry = false;

	castkms_grant_close_revoker(registry, file_state);

	grant = file_state->holder_grant;
	if (!grant)
		return;
	authority = grant->authority;
	castkms_capture_authority_revoke(authority, -EKEYREVOKED);

	mutex_lock(&grant->lock);
	grant->holder_file = NULL;
	mutex_unlock(&grant->lock);

	mutex_lock(&registry->lock);
	if (grant->revoker_file) {
		xa_erase(&grant->revoker_file->revocable_grants, grant->id);
		grant->revoker_file = NULL;
	}
	if (xa_load(&registry->grants, grant->id) == grant) {
		xa_erase(&registry->grants, grant->id);
		put_registry = true;
	}
	mutex_unlock(&registry->lock);

	file_state->holder_grant = NULL;
	if (put_registry)
		castkms_capture_authority_put(authority);
	castkms_capture_authority_put(authority);
}

static void castkms_grant_device_fini(struct drm_device *dev, void *data)
{
	struct castkms_uapi_device *uapi_device = data;
	struct castkms_grant_registry *registry = uapi_device->grant_registry;

	if (!registry)
		return;
	WARN_ON(!xa_empty(&registry->grants));
	xa_destroy(&registry->grants);
	mutex_destroy(&registry->lock);
	uapi_device->grant_registry = NULL;
}

int castkms_grant_device_init(struct drm_device *dev)
{
	struct castkms_uapi_device *uapi_device =
		drm_device_to_castkms_uapi_device(dev);
	struct castkms_grant_registry *registry;

	registry = drmm_kzalloc(dev, sizeof(*registry), GFP_KERNEL);
	if (!registry)
		return -ENOMEM;
	mutex_init(&registry->lock);
	xa_init_flags(&registry->grants, XA_FLAGS_ALLOC);
	registry->next_id = 1;
	uapi_device->grant_registry = registry;

	return drmm_add_action_or_reset(dev, castkms_grant_device_fini,
					uapi_device);
}

void castkms_grant_show_fdinfo(struct drm_printer *p, struct drm_file *file)
{
	struct castkms_capture_grant *grant = castkms_grant_from_file(file);
	struct castkms_capture_authority *authority;
	enum castkms_capture_authority_state state;
	const char *state_name;
	u32 connector_id;
	u32 grant_id;
	u32 rights;
	bool administrative;
	bool delegated;
	bool attached = false;

	if (!grant)
		return;
	authority = grant->authority;

	mutex_lock(&grant->lock);
	grant_id = grant->id;
	connector_id = castkms_capture_authority_connector(authority)->base.id;
	rights = castkms_grant_rights_to_uapi(
		castkms_capture_authority_rights(authority));
	administrative =
		castkms_capture_authority_is_administrative(authority);
	delegated = grant->delegated;
	state = castkms_capture_authority_is_revoked(authority) ?
		CASTKMS_CAPTURE_AUTHORITY_REVOKED : grant->reported_state;
	mutex_unlock(&grant->lock);

	if (state == CASTKMS_CAPTURE_AUTHORITY_PENDING)
		state_name = "pending";
	else if (state == CASTKMS_CAPTURE_AUTHORITY_ACTIVE)
		state_name = "active";
	else if (state == CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_NO_MASTER)
		state_name = "suspended-no-master";
	else if (state == CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_OTHER_MASTER)
		state_name = "suspended-other-master";
	else if (state == CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_FOREIGN_CONTENT)
		state_name = "suspended-foreign-content";
	else
		state_name = "revoked";
	/* Grant-owned attachment arrives with the attachment UAPI. */
	drm_printf(p, "castkms-grant-id:\t%u\n", grant_id);
	drm_printf(p, "castkms-grant-connector:\t%u\n", connector_id);
	drm_printf(p, "castkms-grant-rights:\t0x%x\n", rights);
	drm_printf(p, "castkms-grant-administrative:\t%s\n",
		   administrative ? "yes" : "no");
	drm_printf(p, "castkms-grant-delegated:\t%s\n",
		   delegated ? "yes" : "no");
	drm_printf(p, "castkms-grant-state:\t%s\n", state_name);
	drm_printf(p, "castkms-grant-attached:\t%s\n",
		   attached ? "yes" : "no");
}
