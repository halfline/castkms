/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CASTKMS_CAPTURE_H_
#define _CASTKMS_CAPTURE_H_

#include <linux/mutex.h>
#include <linux/spinlock.h>

struct drm_device;
struct castkms_capture_stream;
struct castkms_output;


/**
 * struct castkms_capture_output - Per-CRTC capture control state
 * @lock: Serializes slow stream ownership changes
 * @stream: File-owned stream with exclusive access to this CRTC
 * @state_lock: Protects the mode snapshot and buffer ownership below
 * @mode_generation: Incremented whenever the CRTC configuration changes
 * @width: Horizontal size of the active mode
 * @height: Vertical size of the active mode
 * @active: Whether the CRTC was active at the latest generation
 */
struct castkms_capture_output {
	struct mutex lock; /* Protects stream. */
	struct castkms_capture_stream *stream;
	spinlock_t state_lock; /* Protects mode and buffer ownership. */
	u64 mode_generation;
	u32 width;
	u32 height;
	bool active;
};

int castkms_capture_query_caps_ioctl(struct drm_device *dev, void *data,
				     struct drm_file *file_priv);
int castkms_capture_start_ioctl(struct drm_device *dev, void *data,
				struct drm_file *file_priv);

int castkms_capture_file_open(struct drm_device *dev,
			      struct drm_file *file_priv);
void castkms_capture_file_close(struct drm_device *dev,
				struct drm_file *file_priv);

int castkms_capture_output_init(struct drm_device *dev,
				struct castkms_output *output);

#endif /* _CASTKMS_CAPTURE_H_ */
