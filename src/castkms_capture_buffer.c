// SPDX-License-Identifier: GPL-2.0-only

#include <linux/completion.h>
#include <linux/dma-fence.h>
#include <linux/err.h>
#include <linux/dma-fence-chain.h>
#include <linux/dma-fence-unwrap.h>
#include <linux/dma-resv.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/slab.h>

#include <drm/drm_rect.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_syncobj.h>

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

struct castkms_capture_fence {
	struct dma_fence base;
	spinlock_t lock; /* Protects @base. */
};

static const char *
castkms_capture_fence_get_driver_name(struct dma_fence *fence)
{
	(void)fence;
	return "castkms";
}

static const char *
castkms_capture_fence_get_timeline_name(struct dma_fence *fence)
{
	(void)fence;
	return "capture";
}

static const struct dma_fence_ops castkms_capture_fence_ops = {
	.get_driver_name = castkms_capture_fence_get_driver_name,
	.get_timeline_name = castkms_capture_fence_get_timeline_name,
};

static struct dma_fence *castkms_capture_fence_create(void)
{
	struct castkms_capture_fence *capture_fence;

	capture_fence = kzalloc_obj(*capture_fence);
	if (!capture_fence)
		return NULL;

	/*
	 * A queued job may be cancelled before an older in-flight job finishes.
	 * Give every producer an independent context so reservation objects retain
	 * both fences instead of replacing the older writer by sequence number.
	 */
	spin_lock_init(&capture_fence->lock);
	dma_fence_init64(&capture_fence->base, &castkms_capture_fence_ops,
			 &capture_fence->lock, dma_fence_context_alloc(1), 1);

	return &capture_fence->base;
}

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
	int status, bool cancelled, bool mode_changed, bool full_damage,
	u64 mode_generation, u64 sequence, ktime_t timestamp)
{
	struct castkms_capture_result *result = &completion->result;

	lockdep_assert_held(&buffer->stream->output->capture.state_lock);
	if (WARN_ON(!buffer->request))
		return;
	WARN_ON(buffer->reuse_callback_armed);

	*result = (struct castkms_capture_result) {};
	result->status = status;
	result->sequence = sequence;
	result->timestamp = timestamp;
	result->mode_generation = mode_generation;
	result->dropped_frames = buffer->dropped_frames;
	result->damage = status ? (struct drm_rect) {} : buffer->damage_clip;
	result->cancelled = cancelled;
	result->mode_changed = mode_changed;
	result->full_damage = !status && full_damage;
	if (!status) {
		result->cursor_serial = buffer->cursor_serial;
		result->cursor_visible = buffer->cursor_visible;
		result->cursor_image_changed = buffer->cursor_image_changed;
		result->cursor_x = buffer->cursor_x;
		result->cursor_y = buffer->cursor_y;
		result->cursor_hotspot_x = buffer->cursor_hotspot_x;
		result->cursor_hotspot_y = buffer->cursor_hotspot_y;
		result->cursor_width = buffer->cursor_width;
		result->cursor_height = buffer->cursor_height;
	}

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

static void
castkms_capture_abort_completion(struct castkms_output *output,
				 struct castkms_capture_completion *completion)
{
	struct dma_fence *dependency = completion->dependency;
	struct dma_fence *fence = completion->fence;
	struct castkms_capture_stream *stream;
	int status = completion->result.status;

	/* Submission failed, so request ownership remains with the caller. */
	completion->request = NULL;
	stream = castkms_capture_begin_delivery(output, completion);
	castkms_capture_signal_fence(fence, status);
	dma_fence_put(dependency);
	*completion = (struct castkms_capture_completion) {};
	if (stream)
		castkms_capture_end_delivery(stream);
}

void castkms_capture_buffer_destroy(struct castkms_capture_buffer *buffer)
{
	if (!buffer)
		return;

	WARN_ON(buffer->state != CASTKMS_CAPTURE_BUFFER_IDLE);
	WARN_ON(buffer->request);
	WARN_ON(buffer->reuse_callback_armed);
	WARN_ON(buffer->completion_fence);
	dma_fence_put(buffer->reuse_fence);
	kfree(buffer->cursor_bitmap);
	castkms_output_buffer_fini(&buffer->output);
	if (buffer->ready_syncobj)
		drm_syncobj_put(buffer->ready_syncobj);
	if (buffer->reuse_syncobj)
		drm_syncobj_put(buffer->reuse_syncobj);
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

static int
castkms_capture_prepare_implicit_sync(struct castkms_capture_buffer *buffer,
				      struct dma_fence **dependency,
				      struct dma_fence **completion_fence)
{
	struct drm_gem_object *obj =
		drm_gem_fb_get_obj(buffer->output.fb, 0);
	struct dma_fence *producer;
	struct dma_fence *reuse = NULL;
	int ret;

	if (WARN_ON(!obj || !obj->resv))
		return -EINVAL;

	producer = castkms_capture_fence_create();
	if (!producer)
		return -ENOMEM;

	ret = dma_resv_lock_interruptible(obj->resv, NULL);
	if (ret)
		goto err_put_producer;

	ret = dma_resv_get_singleton(obj->resv, dma_resv_usage_rw(true),
				     &reuse);
	if (ret)
		goto err_unlock;
	if (reuse && dma_fence_is_signaled(reuse)) {
		dma_fence_put(reuse);
		reuse = NULL;
	}

	ret = dma_resv_reserve_fences(obj->resv, 1);
	if (ret)
		goto err_put_reuse;

	dma_resv_add_fence(obj->resv, producer, DMA_RESV_USAGE_WRITE);
	dma_resv_unlock(obj->resv);

	*dependency = reuse;
	*completion_fence = producer;
	return 0;

err_put_reuse:
	dma_fence_put(reuse);
err_unlock:
	dma_resv_unlock(obj->resv);
err_put_producer:
	dma_fence_put(producer);
	return ret;
}

static int
castkms_capture_get_reuse_fence(struct drm_syncobj *syncobj, u64 point,
				struct dma_fence **reuse_fence)
{
	struct dma_fence *fence;
	int ret;

	*reuse_fence = NULL;
	if (!point)
		return 0;

	fence = drm_syncobj_fence_get(syncobj);
	if (!fence)
		return -EINVAL;

	ret = dma_fence_chain_find_seqno(&fence, point);
	if (ret) {
		dma_fence_put(fence);
		return ret;
	}
	if (!fence)
		fence = dma_fence_get_stub();
	if (dma_fence_is_signaled(fence)) {
		dma_fence_put(fence);
		return 0;
	}

	*reuse_fence = fence;
	return 0;
}

static bool
castkms_capture_ready_point_is_new(struct drm_syncobj *syncobj, u64 point)
{
	struct dma_fence *fence = drm_syncobj_fence_get(syncobj);
	struct dma_fence_chain *chain = to_dma_fence_chain(fence);
	bool is_new = !chain || point > chain->base.seqno;

	dma_fence_put(fence);
	return is_new;
}

static int
castkms_capture_prepare_explicit_sync(struct castkms_capture_buffer *buffer,
				      u64 ready_point, u64 reuse_point,
				      struct dma_fence **dependency,
				      struct dma_fence **completion_fence,
				      struct dma_fence_chain **ready_chain)
{
	struct dma_fence *producer;
	int ret;

	if (!ready_point || ready_point <= buffer->last_ready_point ||
	    !castkms_capture_ready_point_is_new(buffer->ready_syncobj,
						ready_point))
		return -EINVAL;
	if (buffer->last_ready_point &&
	    (!reuse_point || reuse_point <= buffer->last_reuse_point))
		return -EINVAL;

	ret = castkms_capture_get_reuse_fence(buffer->reuse_syncobj,
					      reuse_point, dependency);
	if (ret)
		return ret;

	producer = castkms_capture_fence_create();
	if (!producer) {
		ret = -ENOMEM;
		goto err_put_dependency;
	}

	*ready_chain = dma_fence_chain_alloc();
	if (!*ready_chain) {
		ret = -ENOMEM;
		goto err_put_producer;
	}

	*completion_fence = producer;
	return 0;

err_put_producer:
	dma_fence_put(producer);
err_put_dependency:
	dma_fence_put(*dependency);
	*dependency = NULL;
	return ret;
}

static void castkms_capture_reuse_ready(struct dma_fence *fence,
					struct dma_fence_cb *callback)
{
	struct castkms_capture_buffer *buffer =
		container_of(callback, struct castkms_capture_buffer, reuse_cb);
	struct castkms_capture_output *capture = &buffer->stream->output->capture;
	unsigned long flags;

	(void)fence;
	spin_lock_irqsave(&capture->state_lock, flags);
	if (buffer->reuse_callback_armed &&
	    buffer->state == CASTKMS_CAPTURE_BUFFER_WAITING_REUSE) {
		buffer->reuse_callback_armed = false;
		castkms_capture_buffer_set_state(
			buffer, CASTKMS_CAPTURE_BUFFER_QUEUED);
	}
	spin_unlock_irqrestore(&capture->state_lock, flags);
}

const struct castkms_output_buffer *
castkms_capture_buffer_output(const struct castkms_capture_buffer *buffer)
{
	return &buffer->output;
}

void castkms_capture_buffer_set_damage(struct castkms_capture_buffer *buffer,
				       const struct drm_rect *clip,
				       bool full_damage)
{
	buffer->damage_clip = *clip;
	buffer->full_damage = full_damage;
}

bool castkms_capture_buffer_excludes_cursor(
	const struct castkms_capture_buffer *buffer)
{
	return buffer->stream->exclude_cursor;
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
		buffer->full_damage, capture->mode_generation,
		buffer->sequence, buffer->timestamp);
	spin_unlock_irqrestore(&capture->state_lock, flags);

	castkms_frame_dispatch_put(output, CASTKMS_FRAME_DISPATCH_CLIENT_CAPTURE);
	castkms_capture_deliver_completion(output, &completion);
}

struct castkms_capture_buffer *
castkms_capture_buffer_create(struct castkms_capture_stream *stream,
			      struct drm_framebuffer *fb,
			      struct drm_syncobj *ready_syncobj,
			      struct drm_syncobj *reuse_syncobj,
			      enum castkms_capture_sync_mode sync_mode,
			      u64 mode_generation)
{
	struct castkms_capture_buffer *buffer;
	int ret;

	ret = castkms_capture_stream_validate_mode(stream, mode_generation);
	if (ret)
		return ERR_PTR(ret);
	if ((sync_mode == CASTKMS_CAPTURE_SYNC_IMPLICIT &&
	     (ready_syncobj || reuse_syncobj)) ||
	    (sync_mode == CASTKMS_CAPTURE_SYNC_EXPLICIT &&
	     (!ready_syncobj || !reuse_syncobj)) ||
	    (sync_mode != CASTKMS_CAPTURE_SYNC_IMPLICIT &&
	     sync_mode != CASTKMS_CAPTURE_SYNC_EXPLICIT))
		return ERR_PTR(-EINVAL);
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

	if (ready_syncobj) {
		buffer->ready_syncobj = ready_syncobj;
		drm_syncobj_get(ready_syncobj);
	}
	if (reuse_syncobj) {
		buffer->reuse_syncobj = reuse_syncobj;
		drm_syncobj_get(reuse_syncobj);
	}

	ret = castkms_output_buffer_init(&buffer->output, fb);
	if (ret)
		goto err_destroy;

	ret = castkms_capture_stream_validate_mode(stream, mode_generation);
	if (ret)
		goto err_destroy;

	buffer->mode_generation = stream->mode_generation;
	buffer->sync_mode = sync_mode;
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

bool castkms_capture_buffer_uses_syncobj(
	const struct castkms_capture_buffer *buffer,
	const struct drm_syncobj *syncobj)
{
	return buffer->ready_syncobj == syncobj ||
	       buffer->reuse_syncobj == syncobj;
}

enum castkms_capture_sync_mode castkms_capture_buffer_sync_mode(
	const struct castkms_capture_buffer *buffer)
{
	return buffer->sync_mode;
}

int castkms_capture_buffer_prepare_submit(
	struct castkms_capture_buffer *buffer)
{
	struct castkms_capture_output *capture = &buffer->stream->output->capture;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&capture->state_lock, flags);
	if (buffer->stream->cancel_status) {
		ret = buffer->stream->cancel_status;
	} else if (buffer->state != CASTKMS_CAPTURE_BUFFER_IDLE ||
	    capture->queued_buffer) {
		ret = -EBUSY;
	} else {
		castkms_capture_buffer_set_state(
			buffer, CASTKMS_CAPTURE_BUFFER_PREPARING);
		reinit_completion(&buffer->submit_done);
	}
	spin_unlock_irqrestore(&capture->state_lock, flags);

	return ret;
}

static void
castkms_capture_buffer_abort_submit(struct castkms_capture_buffer *buffer)
{
	struct castkms_capture_output *capture = &buffer->stream->output->capture;
	unsigned long flags;

	spin_lock_irqsave(&capture->state_lock, flags);
	if (!WARN_ON(buffer->state != CASTKMS_CAPTURE_BUFFER_PREPARING))
		castkms_capture_buffer_set_state(
			buffer, CASTKMS_CAPTURE_BUFFER_IDLE);
	complete_all(&buffer->submit_done);
	spin_unlock_irqrestore(&capture->state_lock, flags);
}

static int
castkms_capture_buffer_validate_request(
	const struct castkms_capture_buffer *buffer,
	const struct castkms_capture_request *request)
{
	if (!request || !request->complete)
		return -EINVAL;
	if (buffer->sync_mode == CASTKMS_CAPTURE_SYNC_IMPLICIT &&
	    (request->ready_point || request->reuse_point))
		return -EINVAL;

	return 0;
}

int castkms_capture_buffer_submit_prepared(
	struct castkms_capture_buffer *buffer,
	struct castkms_capture_request *request)
{
	struct castkms_capture_stream *stream = buffer->stream;
	struct castkms_capture_output *capture = &stream->output->capture;
	struct castkms_capture_completion failed_completion = {};
	struct dma_fence_chain *ready_chain = NULL;
	struct dma_fence *completion_fence = NULL;
	struct dma_fence *dependency = NULL;
	struct dma_fence *ready_fence = NULL;
	unsigned long flags;
	bool remove_callback = false;
	int ret;

	ret = castkms_capture_buffer_validate_request(buffer, request);
	if (ret)
		goto out_abort_submit;

	if (buffer->sync_mode == CASTKMS_CAPTURE_SYNC_IMPLICIT)
		ret = castkms_capture_prepare_implicit_sync(buffer, &dependency,
							    &completion_fence);
	else
		ret = castkms_capture_prepare_explicit_sync(buffer,
							    request->ready_point,
							    request->reuse_point,
							    &dependency,
							    &completion_fence,
							    &ready_chain);
	if (ret)
		goto out_abort_submit;
	if (ready_chain) {
		if (dependency)
			ready_fence =
				dma_fence_unwrap_merge(dependency,
						       completion_fence);
		else
			ready_fence = dma_fence_get(completion_fence);
		if (!ready_fence) {
			ret = -ENOMEM;
			goto out_signal_fence;
		}
	}

	ret = castkms_frame_dispatch_get(stream->output,
				   CASTKMS_FRAME_DISPATCH_CLIENT_CAPTURE);
	if (ret)
		goto out_signal_fence;

	if (ready_chain) {
		drm_syncobj_add_point(buffer->ready_syncobj, ready_chain,
				      ready_fence, request->ready_point);
		ready_chain = NULL;
		dma_fence_put(ready_fence);
		ready_fence = NULL;
		buffer->last_ready_point = request->ready_point;
		buffer->last_reuse_point = request->reuse_point;
	}

	/*
	 * Arm an existing dependency before publishing the buffer to vblank or
	 * mode-change cancellation. The callback may transition this private
	 * buffer to QUEUED before publication, but cannot make it globally
	 * visible on its own.
	 */
	spin_lock_irqsave(&capture->state_lock, flags);
	WARN_ON(buffer->state != CASTKMS_CAPTURE_BUFFER_PREPARING);
	buffer->request = request;
	buffer->reuse_fence = dependency;
	buffer->completion_fence = completion_fence;
	buffer->dropped_frames = 0;
	buffer->reuse_callback_armed = !!dependency;
	castkms_capture_buffer_set_state(
		buffer, dependency ? CASTKMS_CAPTURE_BUFFER_WAITING_REUSE :
				     CASTKMS_CAPTURE_BUFFER_QUEUED);
	dependency = NULL;
	completion_fence = NULL;
	spin_unlock_irqrestore(&capture->state_lock, flags);

	if (buffer->reuse_callback_armed) {
		ret = dma_fence_add_callback(buffer->reuse_fence,
					     &buffer->reuse_cb,
					     castkms_capture_reuse_ready);
		if (ret == -ENOENT) {
			spin_lock_irqsave(&capture->state_lock, flags);
			if (buffer->reuse_callback_armed &&
			    buffer->state ==
					CASTKMS_CAPTURE_BUFFER_WAITING_REUSE) {
				buffer->reuse_callback_armed = false;
				castkms_capture_buffer_set_state(
					buffer, CASTKMS_CAPTURE_BUFFER_QUEUED);
			}
			spin_unlock_irqrestore(&capture->state_lock, flags);
			ret = 0;
		} else if (ret) {
			spin_lock_irqsave(&capture->state_lock, flags);
			buffer->reuse_callback_armed = false;
			castkms_capture_buffer_finish(
				buffer, &failed_completion, ret, false, false,
				false, capture->mode_generation, 0, ktime_get());
			spin_unlock_irqrestore(&capture->state_lock, flags);
		}
	}
	if (ret)
		goto out_put_dispatch;

	/*
	 * Publish only after callback setup is complete. output->lock closes the
	 * final validation race with modesets and vblank selection.
	 */
	spin_lock_irqsave(&stream->output->lock, flags);
	spin_lock(&capture->state_lock);
	ret = stream->cancel_status;
	if (!ret)
		ret = castkms_capture_authority_evaluate_stream_status(
			stream->authority, stream->output,
			stream->authority_generation);
	if (!ret && capture->mode_generation != stream->mode_generation) {
		ret = -ESTALE;
	} else if (!capture->active) {
		ret = -ENOLINK;
	} else if (capture->queued_buffer) {
		ret = -EBUSY;
	} else {
		capture->queued_buffer = buffer;
		/* No submitter state is accessed after dropping these locks. */
		complete_all(&buffer->submit_done);
		ret = 0;
	}
	if (ret) {
		remove_callback = buffer->reuse_callback_armed;
		buffer->reuse_callback_armed = false;
		castkms_capture_buffer_finish(
			buffer, &failed_completion, ret, false, false, false,
			capture->mode_generation, 0, ktime_get());
	}
	spin_unlock(&capture->state_lock);
	spin_unlock_irqrestore(&stream->output->lock, flags);
	if (!ret)
		return 0;

	if (remove_callback)
		dma_fence_remove_callback(failed_completion.dependency,
					  &buffer->reuse_cb);
out_put_dispatch:
	castkms_frame_dispatch_put(stream->output,
			     CASTKMS_FRAME_DISPATCH_CLIENT_CAPTURE);
	if (failed_completion.buffer) {
		castkms_capture_abort_completion(stream->output,
					 &failed_completion);
		complete_all(&buffer->submit_done);
		return ret;
	}
out_signal_fence:
	dma_fence_chain_free(ready_chain);
	dma_fence_put(ready_fence);
	castkms_capture_signal_fence(completion_fence, ret);
	dma_fence_put(dependency);
out_abort_submit:
	castkms_capture_buffer_abort_submit(buffer);
	return ret;
}

int
castkms_capture_buffer_submit(struct castkms_capture_buffer *buffer,
			      struct castkms_capture_request *request)
{
	int ret;

	ret = castkms_capture_buffer_validate_request(buffer, request);
	if (ret)
		return ret;
	ret = castkms_capture_buffer_prepare_submit(buffer);
	if (ret)
		return ret;

	return castkms_capture_buffer_submit_prepared(buffer, request);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_buffer_submit);
