/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CASTKMS_CAPTURE_AUTHORITY_H_
#define _CASTKMS_CAPTURE_AUTHORITY_H_

#include <linux/types.h>

struct castkms_capture_authority;
struct castkms_device;
struct castkms_output;
struct drm_atomic_state;
struct drm_connector;
struct drm_device;
struct drm_file;
struct drm_master;

/*
 * Kernel-internal capture rights.  The grant UAPI translates its public bit
 * values to this mask at the ioctl boundary.
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
 * @stop_streams: Stop every client-indexed stream for terminal cleanup
 * @stop_streams_before: Stop streams from generations older than the supplied
 *                       master-cleanup generation
 * @state_changed: Report a non-terminal effective-state transition
 * @revoked: Report terminal revocation after core resources have been stopped
 * @release: Release the owner's wrapper after the final authority reference
 *
 * The authority core owns security state and synchronizes these callbacks with
 * revocation.  An in-kernel client may omit any callback it does not need.
 */
struct castkms_capture_authority_ops {
	void (*stop_streams)(struct castkms_capture_authority *authority,
			     int status, void *data);
	void (*stop_streams_before)(
		struct castkms_capture_authority *authority,
		u64 before_generation, int status, void *data);
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
int castkms_capture_authority_capture_status_locked(
	const struct castkms_capture_authority *authority,
	const struct castkms_output *output);
u64 castkms_capture_authority_stream_generation(
	const struct castkms_capture_authority *authority);
bool castkms_capture_authority_generation_is_stale(
	u64 stream_generation, u64 cleanup_generation);
int castkms_capture_authority_stream_status(
	const struct castkms_capture_authority *authority,
	struct castkms_output *output, u64 stream_generation);
int castkms_capture_authority_stream_status_locked(
	const struct castkms_capture_authority *authority,
	const struct castkms_output *output, u64 stream_generation);
int castkms_capture_authority_stream_status_only(
	const struct castkms_capture_authority *authority,
	u64 stream_generation);

void castkms_capture_authority_stop_connector_streams(
	struct drm_connector *connector,
	struct castkms_capture_authority *skip, int status);

int castkms_capture_authority_device_init(struct castkms_device *castkmsdev);
void castkms_capture_authority_revoke_all(
	struct castkms_device *castkmsdev, int status);
void castkms_capture_authority_master_set(
	struct drm_device *dev, struct drm_file *file_priv, bool from_open);
void castkms_capture_authority_master_drop(
	struct drm_device *dev, struct drm_file *file_priv);
void castkms_capture_authority_master_file_close(
	struct drm_device *dev, struct drm_file *file_priv);
struct drm_master *castkms_capture_authority_current_master_get(
	struct drm_device *dev);
void castkms_capture_authority_publish_content_owners(
	struct drm_atomic_state *state);

bool castkms_capture_owner_is_current(const struct drm_master *capture_owner,
				      const struct drm_master *current_master);
bool castkms_capture_owner_is_active_current(
	struct drm_device *dev, const struct drm_master *capture_owner);
bool castkms_capture_output_content_is_safe_locked(
	const struct castkms_output *output);

#if IS_ENABLED(CONFIG_KUNIT)
enum castkms_capture_authority_state castkms_capture_authority_resolve_state(
	bool permanently_revoked, bool device_shutdown, bool administrative,
	const void *bound_master, const void *current_master,
	bool master_active, bool connector_ready, bool content_safe);
bool castkms_capture_blank_establishes_owner(bool old_state_exists,
					     bool old_had_visible_planes,
					     bool mode_changed,
					     bool active_changed,
					     bool background_changed);
bool castkms_capture_authority_stream_generation_is_current(
	u64 stream_generation, u64 cleanup_generation);
#endif

#endif /* _CASTKMS_CAPTURE_AUTHORITY_H_ */
