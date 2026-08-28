/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CASTKMS_CAPTURE_H_
#define _CASTKMS_CAPTURE_H_

#include <linux/ktime.h>
#include <linux/types.h>

#include <drm/drm_rect.h>

#include "castkms_capture_output.h"

struct drm_crtc_state;
struct drm_device;
struct drm_framebuffer;
struct drm_syncobj;
struct dma_fence;
struct castkms_capture_buffer;
struct castkms_capture_authority;
struct castkms_capture_stream;
struct castkms_crtc_state;
struct castkms_cursor_snapshot;
struct castkms_frame_snapshot;
struct castkms_output;
struct castkms_output_buffer;

#define CASTKMS_CAPTURE_MAX_BUFFERS 8

enum castkms_capture_sync_mode {
	CASTKMS_CAPTURE_SYNC_IMPLICIT,
	CASTKMS_CAPTURE_SYNC_EXPLICIT,
};

/**
 * struct castkms_capture_result - Transport-neutral capture result
 * @status: Zero on success or a negative errno on failure
 * @sequence: Vblank sequence selected for the frame, or zero on cancellation
 * @timestamp: Vblank timestamp, or the cancellation timestamp
 * @mode_generation: Output mode generation at completion
 * @dropped_frames: Frames skipped while this request was queued
 * @damage: Captured damage; empty when @status is nonzero
 * @cancelled: The request was withdrawn instead of reported as a frame
 * @mode_changed: The request was invalidated by an output mode change
 * @full_damage: @damage covers the complete captured frame
 * @cursor_serial: Serial of the cursor image state
 * @cursor_visible: The cursor was visible in the captured frame
 * @cursor_image_changed: Cursor image state changed since the prior frame
 * @cursor_x: Cursor x coordinate
 * @cursor_y: Cursor y coordinate
 * @cursor_hotspot_x: Cursor hotspot x coordinate
 * @cursor_hotspot_y: Cursor hotspot y coordinate
 * @cursor_width: Cursor image width
 * @cursor_height: Cursor image height
 */
struct castkms_capture_result {
	int status;
	u64 sequence;
	ktime_t timestamp;
	u64 mode_generation;
	u32 dropped_frames;
	struct drm_rect damage;
	bool cancelled;
	bool mode_changed;
	bool full_damage;
	u32 cursor_serial;
	bool cursor_visible;
	bool cursor_image_changed;
	s32 cursor_x;
	s32 cursor_y;
	u32 cursor_hotspot_x;
	u32 cursor_hotspot_y;
	u32 cursor_width;
	u32 cursor_height;
};

/**
 * struct castkms_capture_request - One core capture submission
 * @complete: Called exactly once after an accepted request finishes
 * @ready_point: Explicit-sync producer point, or zero for implicit sync
 * @reuse_point: Explicit-sync reuse dependency, or zero for implicit sync
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
	u64 ready_point;
	u64 reuse_point;
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

/**
 * struct castkms_capture_cursor_data - Captured cursor bitmap view
 * @bitmap: Core-owned tightly packed ARGB8888 pixels, or NULL
 * @size: Size of @bitmap in bytes
 * @stride: Bitmap row stride in bytes
 * @width: Bitmap width
 * @height: Bitmap height
 *
 * The view remains valid only while the buffer stays idle and registered.
 */
struct castkms_capture_cursor_data {
	const void *bitmap;
	u32 size;
	u32 stride;
	u32 width;
	u32 height;
};

int castkms_capture_output_init(struct drm_device *dev,
				struct castkms_output *output);
bool castkms_capture_mode_changed(struct castkms_output *output,
				  const struct drm_crtc_state *state,
				  struct castkms_capture_completion *completion);
void castkms_capture_deliver_completion(
	struct castkms_output *output,
	struct castkms_capture_completion *completion);
bool castkms_capture_prepare_frame(struct castkms_output *output,
				   struct castkms_crtc_state *state,
				   u64 sequence, ktime_t timestamp);
const struct castkms_output_buffer *
castkms_capture_buffer_output(const struct castkms_capture_buffer *buffer);
void castkms_capture_buffer_set_damage(struct castkms_capture_buffer *buffer,
				       const struct drm_rect *clip,
				       bool full_damage);
int castkms_capture_buffer_set_cursor(struct castkms_capture_buffer *buffer,
				      const struct castkms_cursor_snapshot *cursor);
bool castkms_capture_buffer_excludes_cursor(
	const struct castkms_capture_buffer *buffer);
void castkms_capture_complete_frame(struct castkms_output *output,
				    struct castkms_capture_buffer *buffer,
				    int status);
void castkms_capture_queue_job(struct castkms_output *output,
			       struct castkms_capture_buffer *buffer,
			       struct castkms_frame_snapshot *snapshot);

/* The caller must hold @authority through begin_output()/end(). */
struct castkms_capture_stream *
castkms_capture_stream_create(struct castkms_output *output,
			      struct castkms_capture_authority *authority,
			      bool exclude_cursor, u64 *mode_generation);
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
			      struct drm_syncobj *ready_syncobj,
			      struct drm_syncobj *reuse_syncobj,
			      enum castkms_capture_sync_mode sync_mode,
			      u64 mode_generation);
int castkms_capture_buffer_remove(struct castkms_capture_stream *stream,
				  struct castkms_capture_buffer *buffer);
bool castkms_capture_buffer_uses_syncobj(const struct castkms_capture_buffer *buffer,
					 const struct drm_syncobj *syncobj);
enum castkms_capture_sync_mode
castkms_capture_buffer_sync_mode(const struct castkms_capture_buffer *buffer);

/*
 * A successful prepare places the buffer in PREPARING. Stream teardown then
 * waits for submission to finish, so the caller may release locks protecting
 * the stream namespace before doing potentially blocking fence preparation.
 * The caller must pair success with exactly one submit_prepared() call.
 */
int castkms_capture_buffer_prepare_submit(
	struct castkms_capture_buffer *buffer);
int castkms_capture_buffer_submit_prepared(
	struct castkms_capture_buffer *buffer,
	struct castkms_capture_request *request);
int castkms_capture_buffer_submit(struct castkms_capture_buffer *buffer,
				  struct castkms_capture_request *request);
int castkms_capture_buffer_get_cursor_data(struct castkms_capture_buffer *buffer,
					   struct castkms_capture_cursor_data *cursor);

#endif /* _CASTKMS_CAPTURE_H_ */
