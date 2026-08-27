/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CASTKMS_CAPTURE_OUTPUT_H_
#define _CASTKMS_CAPTURE_OUTPUT_H_

#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>

struct castkms_capture_buffer;
struct castkms_capture_stream;

/**
 * struct castkms_capture_output - Per-CRTC capture control state
 * @lock: Serializes slow stream ownership changes
 * @stream: Current stream with exclusive access to this CRTC
 * @state_lock: Protects the mode snapshot and buffer ownership below
 * @queued_buffer: Buffer waiting for a future vblank
 * @in_flight_buffer: Buffer currently owned by a frame-dispatch worker
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

#endif /* _CASTKMS_CAPTURE_OUTPUT_H_ */
