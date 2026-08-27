/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CASTKMS_CAPTURE_OWNER_H_
#define _CASTKMS_CAPTURE_OWNER_H_

#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

struct castkms_device;
struct castkms_output;
struct drm_atomic_commit;
struct drm_device;
struct drm_file;
struct drm_master;

/**
 * struct castkms_capture_owner_ops - ownership-change notification
 * @changed: Reconcile consumers after ownership facts change
 *
 * The callback runs from process context without @lock held. Multiple changes
 * may coalesce, so consumers must treat @cleanup_sequence as an observed
 * generation rather than as a count of callback invocations.
 */
struct castkms_capture_owner_ops {
	void (*changed)(void *data, u64 cleanup_sequence);
};

/**
 * struct castkms_capture_owner_state - device-global DRM ownership tracker
 * @lock: Protects the ownership facts and generations below
 * @master: Refcounted current or most recently dropped DRM master
 * @master_file: File which most recently installed @master
 * @transition: Monotonic master/content transition generation
 * @cleanup_sequence: Master-drop generation requiring stream cleanup
 * @master_active: Whether @master is currently installed
 * @shutdown: Whether tracking has stopped for device teardown
 * @work: Deferred notification of ownership changes; not covered by @lock
 * @ops: Immutable notification operations; not covered by @lock
 * @data: Opaque immutable notification callback data; not covered by @lock
 */
struct castkms_capture_owner_state {
	spinlock_t lock; /* Protects ownership facts and generations below. */
	struct drm_master *master;
	struct drm_file *master_file;
	u64 transition;
	u64 cleanup_sequence;
	bool master_active;
	bool shutdown;
	struct work_struct work;
	const struct castkms_capture_owner_ops *ops;
	void *data;
};

/**
 * struct castkms_capture_owner_snapshot - facts used by authority evaluation
 * @master_present: A current or most recently dropped master is known
 * @master_active: The tracked master is currently installed
 * @bound_master_current: The queried bound master is the tracked master
 * @content_safe: The output content belongs to the active tracked master
 * @cleanup_sequence: Current master-drop cleanup generation
 */
struct castkms_capture_owner_snapshot {
	bool master_present;
	bool master_active;
	bool bound_master_current;
	bool content_safe;
	u64 cleanup_sequence;
};

void castkms_capture_owner_device_init(
	struct castkms_device *castkmsdev,
	const struct castkms_capture_owner_ops *ops, void *data);
void castkms_capture_owner_device_fini(struct castkms_device *castkmsdev);

void castkms_capture_owner_snapshot(struct drm_device *dev,
				    const struct drm_master *bound_master,
				    struct castkms_capture_owner_snapshot *snapshot);
/* The caller must hold output->lock. */
void castkms_capture_owner_take_output_snapshot(
	const struct castkms_output *output,
	const struct drm_master *bound_master,
	struct castkms_capture_owner_snapshot *snapshot);

struct drm_master *
castkms_capture_owner_current_master_get(struct drm_device *dev);
bool castkms_capture_owner_is_current(const struct drm_master *capture_owner,
				      const struct drm_master *current_master);
bool castkms_capture_owner_is_active_current(struct drm_device *dev,
					     const struct drm_master *capture_owner);
/* The caller must hold output->lock. */
bool castkms_capture_output_has_safe_content(
	const struct castkms_output *output);

void castkms_capture_owner_publish(struct drm_atomic_commit *state);
void castkms_capture_owner_master_set(struct drm_device *dev,
				      struct drm_file *file_priv, bool from_open);
void castkms_capture_owner_master_drop(struct drm_device *dev,
				       struct drm_file *file_priv);
void castkms_capture_owner_file_close(struct drm_device *dev,
				      struct drm_file *file_priv);

#if IS_ENABLED(CONFIG_KUNIT)
bool castkms_capture_blank_establishes_owner(bool old_state_exists,
					     bool old_had_visible_planes,
					     bool mode_changed,
					     bool active_changed,
					     bool background_changed);
#endif

#endif /* _CASTKMS_CAPTURE_OWNER_H_ */
