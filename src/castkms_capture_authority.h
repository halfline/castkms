/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CASTKMS_CAPTURE_AUTHORITY_H_
#define _CASTKMS_CAPTURE_AUTHORITY_H_

#include <linux/list.h>
#include <linux/types.h>

struct castkms_capture_authority;
struct castkms_device;
struct castkms_output;
struct drm_connector;
struct drm_master;

struct castkms_capture_authority_resource;

/**
 * enum castkms_capture_authority_cleanup_reason - selective resource cleanup
 * @CASTKMS_CAPTURE_AUTHORITY_CLEANUP_MASTER_EPOCH:
 *	A DRM-master cleanup epoch invalidated resources created earlier
 * @CASTKMS_CAPTURE_AUTHORITY_CLEANUP_DISCONNECT:
 *	The authority's connector lost its attached monitor
 */
enum castkms_capture_authority_cleanup_reason {
	CASTKMS_CAPTURE_AUTHORITY_CLEANUP_MASTER_EPOCH,
	CASTKMS_CAPTURE_AUTHORITY_CLEANUP_DISCONNECT,
};

/**
 * struct castkms_capture_authority_resource_ops - core resource cleanup hooks
 * @needs_cleanup: Select resources affected by a non-terminal cleanup reason
 * @suspend: Quiesce a resource for a temporary authority suspension
 * @revoke: Permanently release a resource during terminal authority cleanup
 *
 * Resource callbacks run synchronously under the authority resource lock.
 * @needs_cleanup is optional; resources without it remain registered during
 * selective cleanup. If it returns true, the authority removes the resource
 * and invokes @revoke with the cleanup status.
 *
 * A suspend callback must leave the resource registered.  The authority
 * removes a resource before invoking its revoke callback, so that callback may
 * release the resource allocation.
 */
struct castkms_capture_authority_resource_ops {
	bool (*needs_cleanup)(
		struct castkms_capture_authority_resource *resource,
		enum castkms_capture_authority_cleanup_reason reason,
		u64 generation);
	void (*suspend)(struct castkms_capture_authority_resource *resource,
			int status);
	void (*revoke)(struct castkms_capture_authority_resource *resource,
		       int status);
};

/**
 * struct castkms_capture_authority_resource - registered core resource
 * @link: Private authority resource-list link
 * @authority: Authority owning this resource while registered
 * @ops: Resource cleanup operations
 */
struct castkms_capture_authority_resource {
	struct list_head link;
	struct castkms_capture_authority *authority;
	const struct castkms_capture_authority_resource_ops *ops;
};

/*
 * Kernel-internal capture rights.  A future UAPI adapter can translate public
 * bit values to this mask at its boundary.
 */
enum castkms_capture_authority_right {
	CASTKMS_CAPTURE_AUTHORITY_CAPTURE_PIXELS		= (1U << 0),
	CASTKMS_CAPTURE_AUTHORITY_MANAGE_ATTACHMENT	= (1U << 1),
	CASTKMS_CAPTURE_AUTHORITY_UPDATE_EDID		= (1U << 2),
	CASTKMS_CAPTURE_AUTHORITY_READ_CURSOR		= (1U << 3),
	CASTKMS_CAPTURE_AUTHORITY_MANAGE_CEC		= (1U << 4),
};

#define CASTKMS_CAPTURE_AUTHORITY_RIGHTS_MASK \
	(CASTKMS_CAPTURE_AUTHORITY_CAPTURE_PIXELS | \
	 CASTKMS_CAPTURE_AUTHORITY_MANAGE_ATTACHMENT | \
	 CASTKMS_CAPTURE_AUTHORITY_UPDATE_EDID | \
	 CASTKMS_CAPTURE_AUTHORITY_READ_CURSOR | \
	 CASTKMS_CAPTURE_AUTHORITY_MANAGE_CEC)

enum castkms_capture_authority_state {
	CASTKMS_CAPTURE_AUTHORITY_PENDING,
	CASTKMS_CAPTURE_AUTHORITY_ACTIVE,
	CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_NO_MASTER,
	CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_OTHER_MASTER,
	CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_FOREIGN_CONTENT,
	CASTKMS_CAPTURE_AUTHORITY_REVOKED,
};

/**
 * struct castkms_capture_authority_ops - owner integration hooks
 * @state_changed: Report a non-terminal effective-state transition
 * @revoked: Report terminal revocation after core resources have been stopped
 * @release: Release the owner's wrapper after the final authority reference
 *
 * The authority core owns security state and synchronizes these callbacks with
 * revocation.  An in-kernel client may omit any callback it does not need.
 */
struct castkms_capture_authority_ops {
	void (*state_changed)(struct castkms_capture_authority *authority,
			      enum castkms_capture_authority_state state,
			      int status, void *data);
	void (*revoked)(struct castkms_capture_authority *authority,
			int status, void *data);
	void (*release)(struct castkms_capture_authority *authority, void *data);
};

struct castkms_capture_authority *
castkms_capture_authority_create(
	struct castkms_device *castkmsdev, struct drm_connector *connector,
	struct drm_master *bound_master, u32 rights, bool administrative,
	const struct castkms_capture_authority_ops *ops, void *data);
void castkms_capture_authority_revoke(
	struct castkms_capture_authority *authority, int status);

void castkms_capture_authority_get(
	struct castkms_capture_authority *authority);
void castkms_capture_authority_put(
	struct castkms_capture_authority *authority);

/*
 * Registration must be bracketed by a successful
 * castkms_capture_authority_begin()/end() pair. Unregistration is always
 * permitted and synchronizes with cleanup. A registered resource holds its
 * own authority reference until explicit removal or authority cleanup.
 */
int castkms_capture_authority_register_resource(
	struct castkms_capture_authority *authority,
	struct castkms_capture_authority_resource *resource,
	const struct castkms_capture_authority_resource_ops *ops);
bool castkms_capture_authority_unregister_resource(
	struct castkms_capture_authority *authority,
	struct castkms_capture_authority_resource *resource);

int castkms_capture_authority_begin(
	struct castkms_capture_authority *authority,
	struct drm_connector *connector, u32 rights);
int castkms_capture_authority_begin_output(
	struct castkms_capture_authority *authority,
	struct castkms_output *output, u32 rights);
void castkms_capture_authority_end(
	struct castkms_capture_authority *authority);

struct drm_connector *castkms_capture_authority_connector(
	struct castkms_capture_authority *authority);
u32 castkms_capture_authority_rights(
	const struct castkms_capture_authority *authority);
bool castkms_capture_authority_is_administrative(
	const struct castkms_capture_authority *authority);
bool castkms_capture_authority_is_bound_to_master(
	const struct castkms_capture_authority *authority,
	const struct drm_master *master);
bool castkms_capture_authority_is_active(
	const struct castkms_capture_authority *authority);
bool castkms_capture_authority_is_revoked(
	const struct castkms_capture_authority *authority);
int castkms_capture_authority_lifetime_status(
	const struct castkms_capture_authority *authority);
bool castkms_capture_authority_has_rights(
	const struct castkms_capture_authority *authority, u32 rights);
int castkms_capture_authority_get_state(
	const struct castkms_capture_authority *authority,
	enum castkms_capture_authority_state *state);
int castkms_capture_authority_state_status(
	const struct castkms_capture_authority *authority,
	enum castkms_capture_authority_state state);

int castkms_capture_authority_capture_status(
	const struct castkms_capture_authority *authority,
	struct castkms_output *output);
/* The caller must hold output->lock. */
int castkms_capture_authority_evaluate_capture_status(
	const struct castkms_capture_authority *authority,
	const struct castkms_output *output);
u64 castkms_capture_authority_stream_generation(
	const struct castkms_capture_authority *authority);
bool castkms_capture_authority_generation_is_stale(
	u64 stream_generation, u64 cleanup_generation);
int castkms_capture_authority_stream_status(
	const struct castkms_capture_authority *authority,
	struct castkms_output *output, u64 stream_generation);
/* The caller must hold output->lock. */
int castkms_capture_authority_evaluate_stream_status(
	const struct castkms_capture_authority *authority,
	const struct castkms_output *output, u64 stream_generation);
int castkms_capture_authority_check_stream_continuity(
	const struct castkms_capture_authority *authority,
	u64 stream_generation);

void castkms_capture_authority_cleanup_connector_resources(
	struct drm_connector *connector,
	struct castkms_capture_authority *skip, int status);

int castkms_capture_authority_device_init(struct castkms_device *castkmsdev);
void castkms_capture_authority_revoke_all(
	struct castkms_device *castkmsdev, int status);
#if IS_ENABLED(CONFIG_KUNIT)
enum castkms_capture_authority_state castkms_capture_authority_resolve_state(
	bool permanently_revoked, bool device_shutdown, bool administrative,
	bool master_present, bool master_active, bool bound_master_current,
	bool connector_ready, bool content_safe);
bool castkms_capture_authority_stream_generation_is_current(
	u64 stream_generation, u64 cleanup_generation);
#endif

#endif /* _CASTKMS_CAPTURE_AUTHORITY_H_ */
