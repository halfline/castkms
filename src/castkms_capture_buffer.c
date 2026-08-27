// SPDX-License-Identifier: GPL-2.0-only

#include <linux/completion.h>
#include <linux/dma-fence.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/slab.h>

#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_framebuffer_helper.h>

#include <kunit/visibility.h>

#include "castkms_capture.h"
#include "castkms_capture_authority.h"
#include "castkms_capture_internal.h"
#include "castkms_frame_dispatch.h"
#include "castkms_output.h"
#include "castkms_output_buffer.h"

VISIBLE_IF_KUNIT bool castkms_capture_buffer_state_transition_valid(
	enum castkms_capture_buffer_state from,
	enum castkms_capture_buffer_state to)
{
	switch (from) {
	case CASTKMS_CAPTURE_BUFFER_IDLE:
		return to == CASTKMS_CAPTURE_BUFFER_PREPARING;
	case CASTKMS_CAPTURE_BUFFER_PREPARING:
		return to == CASTKMS_CAPTURE_BUFFER_IDLE ||
		       to == CASTKMS_CAPTURE_BUFFER_WAITING_REUSE ||
		       to == CASTKMS_CAPTURE_BUFFER_QUEUED ||
		       to == CASTKMS_CAPTURE_BUFFER_COMPLETING;
	case CASTKMS_CAPTURE_BUFFER_WAITING_REUSE:
		return to == CASTKMS_CAPTURE_BUFFER_QUEUED ||
		       to == CASTKMS_CAPTURE_BUFFER_COMPLETING;
	case CASTKMS_CAPTURE_BUFFER_QUEUED:
		return to == CASTKMS_CAPTURE_BUFFER_IN_FLIGHT ||
		       to == CASTKMS_CAPTURE_BUFFER_COMPLETING;
	case CASTKMS_CAPTURE_BUFFER_IN_FLIGHT:
		return to == CASTKMS_CAPTURE_BUFFER_QUEUED ||
		       to == CASTKMS_CAPTURE_BUFFER_COMPLETING;
	case CASTKMS_CAPTURE_BUFFER_COMPLETING:
		return to == CASTKMS_CAPTURE_BUFFER_IDLE;
	default:
		return false;
	}
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_buffer_state_transition_valid);

static void castkms_capture_signal_fence(struct dma_fence *fence, int status)
{
	bool fence_cookie;

	if (!fence)
		return;

	fence_cookie = dma_fence_begin_signalling();
	if (status)
		dma_fence_set_error(fence, status);
	dma_fence_signal_timestamp(fence, ktime_get());
	dma_fence_end_signalling(fence_cookie);
	dma_fence_put(fence);
}

void castkms_capture_buffer_finish(
	struct castkms_capture_buffer *buffer,
	struct castkms_capture_completion *completion,
	int status, bool cancelled, bool mode_changed,
	u64 mode_generation, u64 sequence, ktime_t timestamp)
{
	struct castkms_capture_result *result = &completion->result;

	lockdep_assert_held(&buffer->stream->output->capture.state_lock);
	if (WARN_ON(!buffer->request))
		return;
	WARN_ON(buffer->reuse_callback_armed);

	result->status = status;
	result->sequence = sequence;
	result->timestamp = timestamp;
	result->mode_generation = mode_generation;
	result->dropped_frames = buffer->dropped_frames;
	result->cancelled = cancelled;
	result->mode_changed = mode_changed;

	completion->request = buffer->request;
	completion->fence = buffer->completion_fence;
	completion->dependency = buffer->reuse_fence;
	completion->buffer = buffer;
	completion->stream = buffer->stream;
	if (!buffer->stream->active_deliveries++)
		reinit_completion(&buffer->stream->deliveries_done);
	buffer->request = NULL;
	buffer->completion_fence = NULL;
	buffer->reuse_fence = NULL;
	castkms_capture_buffer_set_state(
		buffer, CASTKMS_CAPTURE_BUFFER_COMPLETING);
}

static struct castkms_capture_stream *
castkms_capture_begin_delivery(struct castkms_output *output,
			       struct castkms_capture_completion *completion)
{
	struct castkms_capture_buffer *buffer = completion->buffer;
	struct castkms_capture_stream *stream = completion->stream;
	struct castkms_capture_output *capture = &output->capture;
	unsigned long flags;

	if (!buffer)
		return NULL;

	spin_lock_irqsave(&capture->state_lock, flags);
	if (!WARN_ON(buffer->state != CASTKMS_CAPTURE_BUFFER_COMPLETING))
		castkms_capture_buffer_set_state(
			buffer, CASTKMS_CAPTURE_BUFFER_IDLE);
	spin_unlock_irqrestore(&capture->state_lock, flags);
	completion->buffer = NULL;

	return stream;
}

static void
castkms_capture_end_delivery(struct castkms_capture_stream *stream)
{
	struct castkms_capture_output *capture = &stream->output->capture;
	unsigned long flags;

	spin_lock_irqsave(&capture->state_lock, flags);
	if (!WARN_ON(!stream->active_deliveries) &&
	    !--stream->active_deliveries)
		complete_all(&stream->deliveries_done);
	spin_unlock_irqrestore(&capture->state_lock, flags);
}

void castkms_capture_deliver_completion(
	struct castkms_output *output,
	struct castkms_capture_completion *completion)
{
	struct castkms_capture_request *request = completion->request;
	struct castkms_capture_stream *stream;
	void (*complete)(struct castkms_capture_request *request,
			 const struct castkms_capture_result *result);
	struct dma_fence *dependency = completion->dependency;
	struct dma_fence *fence = completion->fence;
	int status = completion->result.status;

	stream = castkms_capture_begin_delivery(output, completion);
	if (request) {
		complete = request->complete;
		if (!WARN_ON(!complete))
			complete(request, &completion->result);
	}
	/* The callback is published before its producer fence becomes observable. */
	castkms_capture_signal_fence(fence, status);
	dma_fence_put(dependency);
	*completion = (struct castkms_capture_completion) {};
	if (stream)
		castkms_capture_end_delivery(stream);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_deliver_completion);

void castkms_capture_buffer_destroy(struct castkms_capture_buffer *buffer)
{
	if (!buffer)
		return;

	WARN_ON(buffer->state != CASTKMS_CAPTURE_BUFFER_IDLE);
	WARN_ON(buffer->request);
	WARN_ON(buffer->reuse_callback_armed);
	WARN_ON(buffer->completion_fence);
	dma_fence_put(buffer->reuse_fence);
	castkms_output_buffer_fini(&buffer->output);
	kfree(buffer);
}

void castkms_capture_buffer_remove_reuse_callback(
	struct castkms_capture_buffer *buffer, struct dma_fence *dependency)
{
	dma_fence_remove_callback(dependency, &buffer->reuse_cb);
}

static bool
castkms_capture_fb_format_is_supported(const struct drm_framebuffer *fb)
{
	return fb->format->format == DRM_FORMAT_XRGB8888 &&
	       fb->modifier == DRM_FORMAT_MOD_LINEAR;
}

static bool
castkms_capture_fb_is_compatible(const struct castkms_capture_stream *stream,
				 const struct drm_framebuffer *fb)
{
	return fb->width == stream->width && fb->height == stream->height &&
	       fb->format->num_planes == 1 &&
	       castkms_capture_fb_format_is_supported(fb) &&
	       !(fb->flags & DRM_MODE_FB_INTERLACED);
}

static bool castkms_capture_fb_is_local(struct drm_framebuffer *fb)
{
	struct drm_gem_object *obj = drm_gem_fb_get_obj(fb, 0);

	/*
	 * Imported DMA-BUFs need an exporter cache-maintenance operation that
	 * does not wait on the producer fence castkms itself adds below. The
	 * generic helper cannot exclude that fence, so keep the initial
	 * protocol to GEM objects created on this device and exported to its
	 * consumers.
	 */
	return obj && !drm_gem_is_imported(obj);
}

const struct castkms_output_buffer *
castkms_capture_buffer_output(const struct castkms_capture_buffer *buffer)
{
	return &buffer->output;
}

void castkms_capture_complete_frame(struct castkms_output *output,
				    struct castkms_capture_buffer *buffer,
				    int status)
{
	struct castkms_capture_output *capture = &output->capture;
	struct castkms_capture_completion completion = {};
	struct castkms_capture_authority *authority = buffer->stream->authority;
	unsigned long flags;
	u64 authority_generation = buffer->stream->authority_generation;
	bool mode_changed = false;
	int authority_status;

	authority_status = castkms_capture_authority_check_stream_continuity(
		authority, authority_generation);

	spin_lock_irqsave(&capture->state_lock, flags);
	if (WARN_ON(capture->in_flight_buffer != buffer ||
		    buffer->state != CASTKMS_CAPTURE_BUFFER_IN_FLIGHT)) {
		spin_unlock_irqrestore(&capture->state_lock, flags);
		return;
	}

	if (buffer->stream->cancel_status) {
		status = buffer->stream->cancel_status;
	} else if (authority_status) {
		status = authority_status;
	} else if (buffer->mode_generation != capture->mode_generation) {
		status = -ESTALE;
		mode_changed = true;
	}

	capture->in_flight_buffer = NULL;
	castkms_capture_buffer_finish(
		buffer, &completion, status, false, mode_changed,
		capture->mode_generation, buffer->sequence, buffer->timestamp);
	spin_unlock_irqrestore(&capture->state_lock, flags);

	castkms_frame_dispatch_put(output, CASTKMS_FRAME_DISPATCH_CLIENT_CAPTURE);
	castkms_capture_deliver_completion(output, &completion);
}

struct castkms_capture_buffer *
castkms_capture_buffer_create(struct castkms_capture_stream *stream,
			      struct drm_framebuffer *fb,
			      u64 mode_generation)
{
	struct castkms_capture_buffer *buffer;
	int ret;

	ret = castkms_capture_stream_validate_mode(stream, mode_generation);
	if (ret)
		return ERR_PTR(ret);
	if (stream->num_buffers >= CASTKMS_CAPTURE_MAX_BUFFERS)
		return ERR_PTR(-ENOSPC);
	if (!castkms_capture_fb_is_compatible(stream, fb))
		return ERR_PTR(-EINVAL);
	if (!castkms_capture_fb_is_local(fb))
		return ERR_PTR(-EOPNOTSUPP);

	buffer = kzalloc_obj(*buffer);
	if (!buffer)
		return ERR_PTR(-ENOMEM);

	init_completion(&buffer->submit_done);
	complete_all(&buffer->submit_done);
	buffer->stream = stream;

	ret = castkms_output_buffer_init(&buffer->output, fb);
	if (ret)
		goto err_destroy;

	ret = castkms_capture_stream_validate_mode(stream, mode_generation);
	if (ret)
		goto err_destroy;

	buffer->mode_generation = stream->mode_generation;
	list_add_tail(&buffer->link, &stream->buffers);
	stream->num_buffers++;

	return buffer;

err_destroy:
	castkms_capture_buffer_destroy(buffer);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_buffer_create);

int castkms_capture_buffer_remove(struct castkms_capture_stream *stream,
				  struct castkms_capture_buffer *buffer)
{
	struct castkms_capture_output *capture = &stream->output->capture;
	unsigned long flags;
	bool busy;

	if (WARN_ON(buffer->stream != stream))
		return -EINVAL;

	spin_lock_irqsave(&stream->output->lock, flags);
	spin_lock(&capture->state_lock);
	busy = buffer->state != CASTKMS_CAPTURE_BUFFER_IDLE;
	spin_unlock(&capture->state_lock);
	spin_unlock_irqrestore(&stream->output->lock, flags);

	if (busy)
		return -EBUSY;

	list_del(&buffer->link);
	stream->num_buffers--;
	castkms_capture_buffer_destroy(buffer);

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_buffer_remove);
