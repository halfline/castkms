/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CASTKMS_CAPTURE_H_
#define _CASTKMS_CAPTURE_H_

#include <linux/mutex.h>

struct drm_device;
struct castkms_capture_stream;
struct castkms_output;


/**
 * struct castkms_capture_output - Per-CRTC capture control state
 * @lock: Serializes slow stream ownership changes
 * @stream: File-owned stream with exclusive access to this CRTC
 */
struct castkms_capture_output {
	struct mutex lock; /* Protects stream. */
	struct castkms_capture_stream *stream;
};

int castkms_capture_query_caps_ioctl(struct drm_device *dev, void *data,
				     struct drm_file *file_priv);

int castkms_capture_file_open(struct drm_device *dev,
			      struct drm_file *file_priv);
void castkms_capture_file_close(struct drm_device *dev,
				struct drm_file *file_priv);

int castkms_capture_output_init(struct drm_device *dev,
				struct castkms_output *output);

#endif /* _CASTKMS_CAPTURE_H_ */
