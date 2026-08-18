/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CASTKMS_CAPTURE_H_
#define _CASTKMS_CAPTURE_H_

#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>

struct drm_crtc_state;
struct drm_device;
struct drm_edid;
struct drm_pending_event;
struct drm_file;
struct dma_fence;
struct castkms_capture_buffer;
struct castkms_capture_stream;
struct castkms_crtc_state;
struct castkms_output;
struct castkms_output_buffer;

/**
 * struct castkms_capture_completion - Detached capture completion delivery
 * @event: Reserved completion event, if userspace should receive one
 * @fence: Driver-owned producer fence for the completed operation
 * @dependency: Prior reuse dependency retained by the queued buffer
 * @buffer: Buffer kept in the completing state until delivery begins
 * @status: Completion status published through @event and @fence
 *
 * A completion is detached while holding the capture state lock and delivered
 * only after caller-owned spinlocks are released. Delivery returns @buffer to
 * IDLE before signaling @fence or publishing @event. Signaling @fence may
 * invoke callbacks in other subsystems.
 */
struct castkms_capture_completion {
	struct drm_pending_event *event;
	struct dma_fence *fence;
	struct dma_fence *dependency;
	struct castkms_capture_buffer *buffer;
	int status;
};

/**
 * struct castkms_capture_output - Per-CRTC capture control state
 * @lock: Serializes slow stream ownership changes
 * @stream: File-owned stream with exclusive access to this CRTC
 * @state_lock: Protects the mode snapshot and buffer ownership below
 * @queued_buffer: Buffer waiting for a future vblank
 * @in_flight_buffer: Buffer currently owned by a composer worker
 * @mode_generation: Incremented whenever the CRTC configuration changes
 * @width: Horizontal size of the active mode
 * @height: Vertical size of the active mode
 * @active: Whether the CRTC was active at the latest generation
 */
struct castkms_capture_output {
	struct mutex lock; /* Protects stream. */
	struct castkms_capture_stream *stream;
	spinlock_t state_lock; /* Protects mode and buffer ownership. */
	struct castkms_capture_buffer *queued_buffer;
	struct castkms_capture_buffer *in_flight_buffer;
	u64 mode_generation;
	u32 width;
	u32 height;
	bool active;
};

int castkms_capture_query_caps_ioctl(struct drm_device *dev, void *data,
				     struct drm_file *file_priv);
int castkms_capture_start_ioctl(struct drm_device *dev, void *data,
				struct drm_file *file_priv);
int castkms_capture_stop_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file_priv);
int castkms_capture_register_buffer_ioctl(struct drm_device *dev, void *data,
					  struct drm_file *file_priv);
int castkms_capture_unregister_buffer_ioctl(struct drm_device *dev, void *data,
					    struct drm_file *file_priv);
int castkms_capture_queue_buffer_ioctl(struct drm_device *dev, void *data,
				       struct drm_file *file_priv);
int castkms_capture_set_output_edid_ioctl(struct drm_device *dev, void *data,
					  struct drm_file *file_priv);

int castkms_capture_file_open(struct drm_device *dev,
			      struct drm_file *file_priv);
void castkms_capture_file_close(struct drm_device *dev,
				struct drm_file *file_priv);

int castkms_capture_output_init(struct drm_device *dev,
				struct castkms_output *output);
bool castkms_capture_mode_changed(struct castkms_output *output,
				  const struct drm_crtc_state *state,
				  struct castkms_capture_completion *completion);
void castkms_capture_send_completion(struct castkms_output *output,
				     struct castkms_capture_completion *completion);
bool castkms_capture_prepare_frame(struct castkms_output *output,
				   struct castkms_crtc_state *state,
				   u64 sequence, ktime_t timestamp);
const struct castkms_output_buffer *
castkms_capture_buffer_output(const struct castkms_capture_buffer *buffer);
void castkms_capture_complete_frame(struct castkms_output *output,
				    struct castkms_capture_buffer *buffer,
				    int status);

int castkms_capture_edid_parse(const void *raw, u32 size,
			       const struct drm_edid **drm_edid);

#endif /* _CASTKMS_CAPTURE_H_ */
