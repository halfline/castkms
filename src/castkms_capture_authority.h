/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CASTKMS_CAPTURE_AUTHORITY_H_
#define _CASTKMS_CAPTURE_AUTHORITY_H_

#include <linux/types.h>

struct castkms_capture_authority;
struct castkms_device;
struct castkms_output;
struct drm_connector;
struct drm_master;

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
};

/**
 * struct castkms_capture_authority_ops - owner integration hooks
 * @release: Release the owner's wrapper after the final authority reference
 *
 * An in-kernel client may omit the callback when it owns no outer wrapper.
 */
struct castkms_capture_authority_ops {
	void (*release)(struct castkms_capture_authority *authority, void *data);
};

struct castkms_capture_authority *
castkms_capture_authority_create(
	struct castkms_device *castkmsdev, struct drm_connector *connector,
	struct drm_master *bound_master, u32 rights, bool administrative,
	const struct castkms_capture_authority_ops *ops, void *data);

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
bool castkms_capture_authority_is_bound_to_master(
	const struct castkms_capture_authority *authority,
	const struct drm_master *master);
bool castkms_capture_authority_is_active(
	const struct castkms_capture_authority *authority);
bool castkms_capture_authority_has_rights(
	const struct castkms_capture_authority *authority, u32 rights);
int castkms_capture_authority_get_state(
	const struct castkms_capture_authority *authority,
	enum castkms_capture_authority_state *state);
int castkms_capture_authority_state_status(
	enum castkms_capture_authority_state state);

int castkms_capture_authority_capture_status(
	const struct castkms_capture_authority *authority,
	struct castkms_output *output);
/* The caller must hold output->lock. */
int castkms_capture_authority_evaluate_capture_status(
	const struct castkms_capture_authority *authority,
	const struct castkms_output *output);

int castkms_capture_authority_device_init(struct castkms_device *castkmsdev);

#if IS_ENABLED(CONFIG_KUNIT)
enum castkms_capture_authority_state castkms_capture_authority_resolve_state(
	bool administrative, bool master_present, bool master_active,
	bool bound_master_current, bool connector_ready, bool content_safe);
#endif

#endif /* _CASTKMS_CAPTURE_AUTHORITY_H_ */
