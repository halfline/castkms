/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CASTKMS_CAPTURE_H_
#define _CASTKMS_CAPTURE_H_

#include <linux/ktime.h>
#include <linux/types.h>

#include "castkms_capture_output.h"

struct drm_crtc_state;
struct drm_device;
struct drm_framebuffer;
struct dma_fence;
struct castkms_capture_buffer;
struct castkms_capture_authority;
struct castkms_capture_stream;
struct castkms_output;

#define CASTKMS_CAPTURE_MAX_BUFFERS 8

/**
 * struct castkms_capture_result - Transport-neutral capture result
 * @status: Zero on success or a negative errno on failure
 * @sequence: Vblank sequence selected for the frame, or zero on cancellation
 * @timestamp: Vblank timestamp, or the cancellation timestamp
 * @mode_generation: Output mode generation at completion
 * @dropped_frames: Frames skipped while this request was queued
 * @cancelled: The request was withdrawn instead of reported as a frame
 * @mode_changed: The request was invalidated by an output mode change
 */
struct castkms_capture_result {
	int status;
	u64 sequence;
	ktime_t timestamp;
	u64 mode_generation;
	u32 dropped_frames;
	bool cancelled;
	bool mode_changed;
};

/**
 * struct castkms_capture_request - One core capture submission
 * @complete: Called exactly once after an accepted request finishes
 *
 * The caller owns the request until submission succeeds. After a successful
 * submission the capture core owns it through the call to @complete. The
 * callback runs outside capture spinlocks and may release the request; the
 * core does not access it again. The result is valid only for the duration of
 * the callback. It must return promptly: it may neither wait for this request's
 * producer fence, which is signaled immediately afterward, nor synchronously
 * destroy the request's stream.
 */
struct castkms_capture_request {
	void (*complete)(struct castkms_capture_request *request,
			 const struct castkms_capture_result *result);
};

/**
 * struct castkms_capture_completion - Detached core completion delivery
 * @request: Request whose callback receives @result
 * @fence: Driver-owned producer fence for the completed operation
 * @dependency: Prior reuse dependency retained by the queued buffer
 * @buffer: Buffer kept in the completing state until delivery begins
 * @stream: Stream kept alive while completion delivery is active
 * @result: Transport-neutral completion data
 *
 * A completion is detached while holding the capture state lock and delivered
 * only after caller-owned spinlocks are released. Delivery returns @buffer to
 * IDLE, invokes the request callback, and then signals @fence. Those steps may
 * invoke callbacks in other subsystems.
 */
struct castkms_capture_completion {
	struct castkms_capture_request *request;
	struct dma_fence *fence;
	struct dma_fence *dependency;
	struct castkms_capture_buffer *buffer;
	struct castkms_capture_stream *stream;
	struct castkms_capture_result result;
};

int castkms_capture_output_init(struct drm_device *dev,
				struct castkms_output *output);
void castkms_capture_mode_changed(struct castkms_output *output,
				  const struct drm_crtc_state *state);
void castkms_capture_deliver_completion(
	struct castkms_output *output,
	struct castkms_capture_completion *completion);

/* The caller must hold @authority through begin_output()/end(). */
struct castkms_capture_stream *
castkms_capture_stream_create(struct castkms_output *output,
			      struct castkms_capture_authority *authority,
			      u64 *mode_generation);
int castkms_capture_stream_attach(struct castkms_capture_stream *stream);
void castkms_capture_stream_destroy(struct castkms_capture_stream *stream,
				    int status);
struct castkms_output *
castkms_capture_stream_output(const struct castkms_capture_stream *stream);
bool castkms_capture_stream_has_authority(
	const struct castkms_capture_stream *stream,
	const struct castkms_capture_authority *authority);
u64 castkms_capture_stream_authority_generation(
	const struct castkms_capture_stream *stream);
int castkms_capture_stream_status(const struct castkms_capture_stream *stream);
int castkms_capture_stream_validate_mode(
	const struct castkms_capture_stream *stream, u64 mode_generation);

struct castkms_capture_buffer *
castkms_capture_buffer_create(struct castkms_capture_stream *stream,
			      struct drm_framebuffer *fb,
			      u64 mode_generation);
int castkms_capture_buffer_remove(struct castkms_capture_stream *stream,
				  struct castkms_capture_buffer *buffer);
#endif /* _CASTKMS_CAPTURE_H_ */
