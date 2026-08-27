/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CASTKMS_CAPTURE_INTERNAL_H_
#define _CASTKMS_CAPTURE_INTERNAL_H_

#include <linux/completion.h>
#include <linux/dma-fence.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/types.h>

#include "castkms_capture.h"
#include "castkms_output_buffer.h"

struct castkms_capture_stream {
	struct castkms_output *output;
	struct castkms_capture_authority *authority;
	struct list_head buffers;
	struct completion deliveries_done;
	u64 authority_generation;
	u64 mode_generation;
	u32 width;
	u32 height;
	u32 num_buffers;
	u32 active_deliveries;
	bool active;
	bool attached;
	int cancel_status;
};

enum castkms_capture_buffer_state {
	CASTKMS_CAPTURE_BUFFER_IDLE,
	CASTKMS_CAPTURE_BUFFER_PREPARING,
	CASTKMS_CAPTURE_BUFFER_WAITING_REUSE,
	CASTKMS_CAPTURE_BUFFER_QUEUED,
	CASTKMS_CAPTURE_BUFFER_IN_FLIGHT,
	CASTKMS_CAPTURE_BUFFER_COMPLETING,
	CASTKMS_CAPTURE_BUFFER_STATE_COUNT,
};

struct castkms_capture_buffer {
	struct castkms_capture_stream *stream;
	struct list_head link;
	struct castkms_output_buffer output;
	struct drm_syncobj *ready_syncobj;
	struct drm_syncobj *reuse_syncobj;
	struct castkms_capture_request *request;
	struct dma_fence *reuse_fence;
	struct dma_fence_cb reuse_cb;
	struct dma_fence *completion_fence;
	struct completion submit_done;
	ktime_t timestamp;
	u64 sequence;
	u64 mode_generation;
	u32 dropped_frames;
	enum castkms_capture_sync_mode sync_mode;
	bool reuse_callback_armed;
	enum castkms_capture_buffer_state state;
};

bool castkms_capture_buffer_state_transition_valid(
	enum castkms_capture_buffer_state from,
	enum castkms_capture_buffer_state to);

/* The caller must hold the capture state lock. */
static inline void castkms_capture_buffer_set_state(
	struct castkms_capture_buffer *buffer,
	enum castkms_capture_buffer_state state)
{
	WARN_ON(!castkms_capture_buffer_state_transition_valid(buffer->state,
							       state));
	buffer->state = state;
}

/* The caller must hold the capture state lock. */
void castkms_capture_buffer_finish(
	struct castkms_capture_buffer *buffer,
	struct castkms_capture_completion *completion,
	int status, bool cancelled, bool mode_changed,
	u64 mode_generation, u64 sequence, ktime_t timestamp);
void castkms_capture_buffer_destroy(struct castkms_capture_buffer *buffer);
void castkms_capture_buffer_remove_reuse_callback(
	struct castkms_capture_buffer *buffer, struct dma_fence *dependency);

#endif /* _CASTKMS_CAPTURE_INTERNAL_H_ */
