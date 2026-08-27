// SPDX-License-Identifier: GPL-2.0-only

#include <linux/err.h>
#include <linux/kref.h>
#include <linux/slab.h>

#include <drm/drm_auth.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_managed.h>

#include <kunit/visibility.h>

#include "castkms_capture_authority.h"
#include "castkms_capture_owner.h"
#include "castkms_drv.h"
#include "castkms_output.h"

/**
 * struct castkms_capture_authority - kernel-native capture authorization
 * @ref: Lifetime held by clients
 * @device: Owning CastKMS device
 * @connector: Refcounted connector scoped by this authority
 * @bound_master: Refcounted DRM master for a normal authority, or NULL
 * @ops: Optional owner integration hooks
 * @data: Opaque owner data passed to @ops
 * @lock: Serializes authorization checks
 * @rights: Immutable CASTKMS_CAPTURE_AUTHORITY_* mask
 * @administrative: Authority follows safe content across master handoffs
 */
struct castkms_capture_authority {
	struct kref ref;
	struct castkms_device *device;
	struct drm_connector *connector;
	struct drm_master *bound_master;
	const struct castkms_capture_authority_ops *ops;
	void *data;
	struct mutex lock; /* Serializes authority use. */
	u32 rights;
	bool administrative;
};

VISIBLE_IF_KUNIT enum castkms_capture_authority_state
castkms_capture_authority_resolve_state(
	bool administrative, bool master_present, bool master_active,
	bool bound_master_current, bool connector_ready, bool content_safe)
{
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

	if (authority->bound_master)
		drm_master_put(&authority->bound_master);
	drm_connector_put(authority->connector);
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

static int castkms_capture_authority_get_routed_output(
	struct drm_connector *connector, struct castkms_output **output)
{
	struct drm_modeset_lock *connection_lock =
		&connector->dev->mode_config.connection_mutex;
	struct drm_crtc *crtc = NULL;
	int ret;

	*output = NULL;
	ret = drm_modeset_lock_single_interruptible(connection_lock);
	if (ret)
		return ret;
	if (connector->state)
		crtc = connector->state->crtc;
	drm_modeset_unlock(connection_lock);

	if (crtc)
		*output = drm_crtc_to_castkms_output(crtc);
	return 0;
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

	ret = castkms_capture_authority_get_routed_output(authority->connector,
							  &output);
	if (ret)
		return ret;
	connector_ready = !!output;

	if (output) {
		spin_lock_irqsave(&output->lock, flags);
		castkms_capture_owner_take_output_snapshot(
			output, authority->bound_master, &ownership);
		spin_unlock_irqrestore(&output->lock, flags);
	} else {
		castkms_capture_owner_snapshot(&castkmsdev->drm,
					       authority->bound_master,
					       &ownership);
	}
	*state = castkms_capture_authority_resolve_state(
		authority->administrative, ownership.master_present,
		ownership.master_active, ownership.bound_master_current,
		connector_ready, ownership.content_safe);

	return 0;
}

int castkms_capture_authority_state_status(
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
	status = castkms_capture_authority_get_routed_output(
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

int castkms_capture_authority_begin(
	struct castkms_capture_authority *authority,
	struct drm_connector *connector, u32 rights)
{
	int status;

	if (!authority)
		return -EACCES;
	(void)connector;

	mutex_lock(&authority->lock);
	status = castkms_capture_authority_status(authority);
	if (!status && !castkms_capture_authority_has_rights(authority, rights))
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

struct castkms_capture_authority *
castkms_capture_authority_create(
	struct castkms_device *castkmsdev, struct drm_connector *connector,
	struct drm_master *bound_master, u32 rights, bool administrative,
	const struct castkms_capture_authority_ops *ops, void *data)
{
	struct castkms_capture_authority *authority;

	if (!rights || rights & ~CASTKMS_CAPTURE_AUTHORITY_RIGHTS_MASK)
		return ERR_PTR(-EINVAL);
	if (!connector ||
	    connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK)
		return ERR_PTR(-ENOENT);
	if (!administrative && !bound_master)
		return ERR_PTR(-EACCES);

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

	return authority;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_authority_create);

static void castkms_capture_authority_device_fini(
	struct drm_device *dev, void *data)
{
	struct castkms_device *castkmsdev = data;

	(void)dev;
	castkms_capture_owner_device_fini(castkmsdev);
}

int castkms_capture_authority_device_init(struct castkms_device *castkmsdev)
{
	castkms_capture_owner_device_init(castkmsdev, NULL, NULL);

	return drmm_add_action_or_reset(&castkmsdev->drm,
					castkms_capture_authority_device_fini,
					castkmsdev);
}
