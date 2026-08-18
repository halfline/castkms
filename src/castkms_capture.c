// SPDX-License-Identifier: GPL-2.0-only

#include <linux/build_bug.h>
#include <linux/completion.h>
#include <linux/dma-fence.h>
#include <linux/dma-fence-chain.h>
#include <linux/dma-fence-unwrap.h>
#include <linux/dma-resv.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/util_macros.h>
#include <linux/xarray.h>

#include <drm/castkms_drm.h>
#include <drm/drm_crtc.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_syncobj.h>

#include "castkms_capture.h"
#include "castkms_drv.h"
#include "castkms_output_buffer.h"

#define CASTKMS_CAPTURE_MAX_BUFFERS 8

struct castkms_capture_file {
	struct mutex lock; /* Protects streams. */
	struct xarray streams;
};

struct castkms_capture_stream {
	struct castkms_output *output;
	struct xarray buffers;
	u64 mode_generation;
	u32 width;
	u32 height;
	u32 id;
	u32 num_buffers;
	bool active;
};

enum castkms_capture_buffer_state {
	CASTKMS_CAPTURE_BUFFER_IDLE,
	CASTKMS_CAPTURE_BUFFER_WAITING_REUSE,
	CASTKMS_CAPTURE_BUFFER_QUEUED,
	CASTKMS_CAPTURE_BUFFER_IN_FLIGHT,
	CASTKMS_CAPTURE_BUFFER_COMPLETING,
};

struct castkms_capture_pending_event {
	struct drm_pending_event pending;
	struct drm_event_castkms_capture_frame event;
};

struct castkms_capture_buffer {
	struct castkms_capture_stream *stream;
	struct castkms_output_buffer output;
	struct drm_syncobj *ready_syncobj;
	struct drm_syncobj *reuse_syncobj;
	struct castkms_capture_pending_event *pending_event;
	struct dma_fence *reuse_fence;
	struct dma_fence_cb reuse_cb;
	struct dma_fence *completion_fence;
	struct completion delivery_done;
	ktime_t timestamp;
	u64 sequence;
	u64 user_data;
	u64 mode_generation;
	u64 last_ready_point;
	u64 last_reuse_point;
	u32 dropped_frames;
	u32 sync_mode;
	u32 id;
	bool reuse_callback_armed;
	enum castkms_capture_buffer_state state;
};

struct castkms_capture_fence {
	struct dma_fence base;
	spinlock_t lock; /* Protects @base. */
};

static const struct drm_castkms_capture_format castkms_capture_formats[] = {
	{
		.format = DRM_FORMAT_XRGB8888,
		.modifier = DRM_FORMAT_MOD_LINEAR,
	},
};

static_assert(sizeof(struct drm_castkms_capture_format) == 16);
static_assert(sizeof(struct drm_castkms_capture_query_caps) == 40);
static_assert(sizeof(struct drm_castkms_capture_start) == 24);
static_assert(sizeof(struct drm_castkms_capture_stop) == 16);
static_assert(sizeof(struct drm_castkms_capture_register_buffer) == 32);
static_assert(sizeof(struct drm_castkms_capture_unregister_buffer) == 16);
static_assert(sizeof(struct drm_castkms_capture_queue_buffer) == 48);
static_assert(sizeof(struct drm_event_castkms_capture_frame) == 80);
static_assert(offsetof(struct drm_event_castkms_capture_frame, reserved) == 76);
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

static void
castkms_capture_finish_locked(struct castkms_capture_buffer *buffer,
			      struct castkms_capture_completion *completion,
			      int status, u32 flags, u64 mode_generation,
			      u64 sequence, ktime_t timestamp)
{
	struct castkms_capture_pending_event *pending = buffer->pending_event;
	struct drm_event_castkms_capture_frame *event;

	if (WARN_ON(!pending))
		return;
	WARN_ON(buffer->reuse_callback_armed);

	event = &pending->event;
	event->user_data = buffer->user_data;
	event->sequence = sequence;
	event->timestamp_ns = ktime_to_ns(timestamp);
	event->mode_generation = mode_generation;
	event->stream_id = buffer->stream->id;
	event->buffer_id = buffer->id;
	event->status = status;
	event->flags = flags;
	event->dropped_frames = buffer->dropped_frames;
	event->damage_x = 0;
	event->damage_y = 0;
	event->damage_width = status ? 0 : buffer->stream->width;
	event->damage_height = status ? 0 : buffer->stream->height;

	completion->event = &pending->pending;
	completion->fence = buffer->completion_fence;
	completion->dependency = buffer->reuse_fence;
	completion->buffer = buffer;
	completion->status = status;
	buffer->pending_event = NULL;
	buffer->completion_fence = NULL;
	buffer->reuse_fence = NULL;
	buffer->state = CASTKMS_CAPTURE_BUFFER_COMPLETING;
}

static void
castkms_capture_finish_delivery(struct castkms_output *output,
				struct castkms_capture_completion *completion)
{
	struct castkms_capture_buffer *buffer = completion->buffer;
	struct castkms_capture_output *capture = &output->capture;
	unsigned long flags;

	if (buffer) {
		spin_lock_irqsave(&capture->state_lock, flags);
		if (!WARN_ON(buffer->state !=
			     CASTKMS_CAPTURE_BUFFER_COMPLETING))
			buffer->state = CASTKMS_CAPTURE_BUFFER_IDLE;
		complete_all(&buffer->delivery_done);
		spin_unlock_irqrestore(&capture->state_lock, flags);
	}
	*completion = (struct castkms_capture_completion) {};
}

void castkms_capture_send_completion(struct castkms_output *output,
				     struct castkms_capture_completion *completion)
{
	struct drm_device *dev = output->crtc.dev;
	struct drm_pending_event *event = completion->event;
	struct dma_fence *dependency = completion->dependency;
	struct dma_fence *fence = completion->fence;
	int status = completion->status;
	unsigned long flags;

	if (event) {
		/*
		 * Publish the event before waking a file-close waiter. Holding the
		 * event lock across the IDLE transition prevents postclose event
		 * cleanup from freeing the detached event first.
		 */
		spin_lock_irqsave(&dev->event_lock, flags);
		castkms_capture_finish_delivery(output, completion);
		drm_send_event_locked(dev, event);
		spin_unlock_irqrestore(&dev->event_lock, flags);
	} else {
		castkms_capture_finish_delivery(output, completion);
	}

	/* The producer fence is the ownership boundary promised by the UAPI. */
	castkms_capture_signal_fence(fence, status);
	dma_fence_put(dependency);
}

static void
castkms_capture_cancel_completion(struct castkms_output *output,
				  struct castkms_capture_completion *completion)
{
	struct drm_pending_event *event = completion->event;
	struct dma_fence *dependency = completion->dependency;
	struct dma_fence *fence = completion->fence;
	int status = completion->status;

	/* Retire the reserved event before delivery can wake file teardown. */
	if (event)
		drm_event_cancel_free(output->crtc.dev, event);
	castkms_capture_finish_delivery(output, completion);
	castkms_capture_signal_fence(fence, status);
	dma_fence_put(dependency);
}

static void
castkms_capture_buffer_destroy(struct castkms_capture_buffer *buffer)
{
	if (!buffer)
		return;

	WARN_ON(buffer->state != CASTKMS_CAPTURE_BUFFER_IDLE);
	WARN_ON(buffer->pending_event);
	WARN_ON(buffer->reuse_callback_armed);
	WARN_ON(buffer->completion_fence);
	dma_fence_put(buffer->reuse_fence);
	castkms_output_buffer_fini(&buffer->output);
	if (buffer->ready_syncobj)
		drm_syncobj_put(buffer->ready_syncobj);
	if (buffer->reuse_syncobj)
		drm_syncobj_put(buffer->reuse_syncobj);
	kfree(buffer);
}

static void castkms_capture_stream_cancel(struct castkms_capture_stream *stream)
{
	struct castkms_capture_output *capture = &stream->output->capture;
	struct castkms_capture_completion completion = {};
	struct castkms_capture_buffer *queued_buffer = NULL;
	struct castkms_capture_buffer *buffer;
	unsigned long flags;
	unsigned long id;
	bool in_flight = false;
	bool remove_callback = false;
	bool put_composer = false;

	/*
	 * Serialize against vblank selection through output->lock. Once that
	 * lock is released, an in-flight buffer's work has been queued and can
	 * be flushed without racing a late queue_work().
	 */
	spin_lock_irqsave(&stream->output->lock, flags);
	spin_lock(&capture->state_lock);
	buffer = capture->queued_buffer;
	if (buffer && buffer->stream == stream) {
		queued_buffer = buffer;
		capture->queued_buffer = NULL;
		remove_callback = buffer->reuse_callback_armed;
		buffer->reuse_callback_armed = false;
		castkms_capture_finish_locked(buffer, &completion, -ECANCELED, 0,
					      capture->mode_generation, 0,
					      ktime_get());
		put_composer = true;
	}

	buffer = capture->in_flight_buffer;
	if (buffer && buffer->stream == stream)
		in_flight = true;
	spin_unlock(&capture->state_lock);
	spin_unlock_irqrestore(&stream->output->lock, flags);

	if (remove_callback)
		dma_fence_remove_callback(completion.dependency,
					  &queued_buffer->reuse_cb);
	if (put_composer)
		castkms_composer_put(stream->output,
				     CASTKMS_COMPOSER_CLIENT_CAPTURE);
	castkms_capture_cancel_completion(stream->output, &completion);
	if (in_flight)
		flush_workqueue(stream->output->composer_workq);
	xa_for_each(&stream->buffers, id, buffer)
		wait_for_completion(&buffer->delivery_done);
}

static void castkms_capture_stream_detach(struct castkms_capture_stream *stream)
{
	struct castkms_capture_output *capture = &stream->output->capture;

	mutex_lock(&capture->lock);
	if (WARN_ON(capture->stream != stream)) {
		mutex_unlock(&capture->lock);
		return;
	}
	capture->stream = NULL;
	mutex_unlock(&capture->lock);
}
static void castkms_capture_stream_destroy(struct castkms_capture_stream *stream)
{
	struct castkms_capture_buffer *buffer;
	unsigned long id;

	castkms_capture_stream_cancel(stream);
	xa_for_each(&stream->buffers, id, buffer)
		castkms_capture_buffer_destroy(buffer);
	xa_destroy(&stream->buffers);

	castkms_capture_stream_detach(stream);
	kfree(stream);
}

static void
castkms_capture_stream_snapshot_mode(struct castkms_capture_stream *stream)
{
	struct castkms_capture_output *capture = &stream->output->capture;
	unsigned long flags;

	spin_lock_irqsave(&capture->state_lock, flags);
	stream->mode_generation = capture->mode_generation;
	stream->width = capture->width;
	stream->height = capture->height;
	stream->active = capture->active;
	spin_unlock_irqrestore(&capture->state_lock, flags);
}

static int
castkms_capture_validate_mode(const struct castkms_capture_stream *stream,
			      u64 mode_generation)
{
	struct castkms_capture_output *capture = &stream->output->capture;
	unsigned long flags;
	int ret = 0;

	if (mode_generation != stream->mode_generation)
		return -ESTALE;

	spin_lock_irqsave(&capture->state_lock, flags);
	if (capture->mode_generation != stream->mode_generation)
		ret = -ESTALE;
	else if (!stream->active)
		ret = -ENOLINK;
	spin_unlock_irqrestore(&capture->state_lock, flags);

	return ret;
}

static bool
castkms_capture_fb_format_is_supported(const struct drm_framebuffer *fb)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(castkms_capture_formats); i++)
		if (fb->format->format == castkms_capture_formats[i].format &&
		    fb->modifier == castkms_capture_formats[i].modifier)
			return true;

	return false;
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

int castkms_capture_file_open(struct drm_device *dev,
			      struct drm_file *file_priv)
{
	struct castkms_capture_file *capture_file;

	capture_file = kzalloc_obj(*capture_file);
	if (!capture_file)
		return -ENOMEM;

	mutex_init(&capture_file->lock);
	xa_init_flags(&capture_file->streams, XA_FLAGS_ALLOC);
	file_priv->driver_priv = capture_file;

	return 0;
}

void castkms_capture_file_close(struct drm_device *dev,
				struct drm_file *file_priv)
{
	struct castkms_capture_file *capture_file = file_priv->driver_priv;
	struct castkms_capture_stream *stream;
	unsigned long id;

	mutex_lock(&capture_file->lock);
	xa_for_each(&capture_file->streams, id, stream) {
		castkms_capture_stream_destroy(stream);
	}
	xa_destroy(&capture_file->streams);
	mutex_unlock(&capture_file->lock);
	mutex_destroy(&capture_file->lock);

	kfree(capture_file);
	file_priv->driver_priv = NULL;
}

int castkms_capture_output_init(struct drm_device *dev,
				struct castkms_output *output)
{
	int ret;

	ret = drmm_mutex_init(dev, &output->capture.lock);
	if (ret)
		return ret;

	output->capture.stream = NULL;
	spin_lock_init(&output->capture.state_lock);
	output->capture.queued_buffer = NULL;
	output->capture.in_flight_buffer = NULL;
	output->capture.mode_generation = 1;
	output->capture.width = 0;
	output->capture.height = 0;
	output->capture.active = false;

	return 0;
}

bool castkms_capture_mode_changed(struct castkms_output *output,
				  const struct drm_crtc_state *state,
				  struct castkms_capture_completion *completion)
{
	struct castkms_capture_output *capture = &output->capture;
	struct castkms_capture_buffer *buffer;
	unsigned long flags;
	u32 event_flags = DRM_CASTKMS_CAPTURE_FRAME_MODE_CHANGED;
	bool remove_callback = false;
	bool cancelled = false;

	*completion = (struct castkms_capture_completion) {};
	spin_lock_irqsave(&capture->state_lock, flags);
	capture->mode_generation++;
	capture->active = state->active;
	capture->width = state->active ? state->mode.hdisplay : 0;
	capture->height = state->active ? state->mode.vdisplay : 0;

	buffer = capture->queued_buffer;
	if (buffer) {
		capture->queued_buffer = NULL;
		remove_callback = buffer->reuse_callback_armed;
		buffer->reuse_callback_armed = false;
		castkms_capture_finish_locked(buffer, completion, -ESTALE,
					      event_flags,
					      capture->mode_generation, 0,
					      ktime_get());
		cancelled = true;
	}
	spin_unlock_irqrestore(&capture->state_lock, flags);

	if (remove_callback)
		dma_fence_remove_callback(completion->dependency,
					  &buffer->reuse_cb);

	return cancelled;
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
		buffer->state = CASTKMS_CAPTURE_BUFFER_QUEUED;
	}
	spin_unlock_irqrestore(&capture->state_lock, flags);
}

bool castkms_capture_prepare_frame(struct castkms_output *output,
				   struct castkms_crtc_state *state,
				   u64 sequence, ktime_t timestamp)
{
	struct castkms_capture_output *capture = &output->capture;
	struct castkms_capture_buffer *buffer;
	unsigned long flags;

	spin_lock_irqsave(&capture->state_lock, flags);
	buffer = capture->queued_buffer;
	if (!buffer) {
		spin_unlock_irqrestore(&capture->state_lock, flags);
		return false;
	}
	if (capture->in_flight_buffer ||
	    buffer->state != CASTKMS_CAPTURE_BUFFER_QUEUED ||
	    buffer->mode_generation != capture->mode_generation ||
	    !capture->active) {
		if (buffer->dropped_frames != U32_MAX)
			buffer->dropped_frames++;
		spin_unlock_irqrestore(&capture->state_lock, flags);
		return false;
	}

	capture->queued_buffer = NULL;
	capture->in_flight_buffer = buffer;
	buffer->sequence = sequence;
	buffer->timestamp = timestamp;
	buffer->state = CASTKMS_CAPTURE_BUFFER_IN_FLIGHT;
	spin_unlock_irqrestore(&capture->state_lock, flags);

	spin_lock_irqsave(&output->composer_lock, flags);
	if (WARN_ON(state->capture_pending || state->active_capture)) {
		spin_unlock_irqrestore(&output->composer_lock, flags);
		spin_lock_irqsave(&capture->state_lock, flags);
		capture->in_flight_buffer = NULL;
		capture->queued_buffer = buffer;
		buffer->state = CASTKMS_CAPTURE_BUFFER_QUEUED;
		spin_unlock_irqrestore(&capture->state_lock, flags);
		return false;
	}
	state->active_capture = buffer;
	state->capture_pending = true;
	spin_unlock_irqrestore(&output->composer_lock, flags);

	return true;
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
	unsigned long flags;
	u32 event_flags = 0;

	spin_lock_irqsave(&capture->state_lock, flags);
	if (WARN_ON(capture->in_flight_buffer != buffer ||
		    buffer->state != CASTKMS_CAPTURE_BUFFER_IN_FLIGHT)) {
		spin_unlock_irqrestore(&capture->state_lock, flags);
		return;
	}

	if (buffer->mode_generation != capture->mode_generation) {
		status = -ESTALE;
		event_flags |= DRM_CASTKMS_CAPTURE_FRAME_MODE_CHANGED;
	} else if (!status) {
		event_flags |= DRM_CASTKMS_CAPTURE_FRAME_FULL_DAMAGE;
	}

	capture->in_flight_buffer = NULL;
	castkms_capture_finish_locked(buffer, &completion, status, event_flags,
				      capture->mode_generation, buffer->sequence,
				      buffer->timestamp);
	spin_unlock_irqrestore(&capture->state_lock, flags);

	castkms_composer_put(output, CASTKMS_COMPOSER_CLIENT_CAPTURE);
	castkms_capture_send_completion(output, &completion);
}

int castkms_capture_query_caps_ioctl(struct drm_device *dev, void *data,
				     struct drm_file *file_priv)
{
	struct drm_castkms_capture_query_caps *args = data;
	void __user *formats = u64_to_user_ptr(args->formats_ptr);
	u32 capacity = args->format_count;

	if (args->reserved)
		return -EINVAL;

	if (!drm_crtc_find(dev, file_priv, args->crtc_id))
		return -ENOENT;

	args->uapi_major = DRM_CASTKMS_CAPTURE_UAPI_MAJOR;
	args->uapi_minor = DRM_CASTKMS_CAPTURE_UAPI_MINOR;
	args->flags = 0;
	args->flags |= DRM_CASTKMS_CAPTURE_CAP_IMPLICIT_SYNC;
	if (drm_core_check_feature(dev, DRIVER_SYNCOBJ_TIMELINE))
		args->flags |= DRM_CASTKMS_CAPTURE_CAP_SYNCOBJ_TIMELINE;
	args->max_registered_buffers = CASTKMS_CAPTURE_MAX_BUFFERS;
	args->format_count = ARRAY_SIZE(castkms_capture_formats);

	if (capacity >= ARRAY_SIZE(castkms_capture_formats) &&
	    copy_to_user(formats, castkms_capture_formats,
			 sizeof(castkms_capture_formats)))
		return -EFAULT;

	return 0;
}

int castkms_capture_start_ioctl(struct drm_device *dev, void *data,
				struct drm_file *file_priv)
{
	struct drm_castkms_capture_start *args = data;
	struct castkms_capture_file *capture_file = file_priv->driver_priv;
	struct castkms_capture_stream *stream;
	struct castkms_output *output;
	struct drm_crtc *crtc;
	u32 stream_id;
	int ret;

	if (args->flags != DRM_CASTKMS_CAPTURE_START_EXCLUSIVE ||
	    args->reserved)
		return -EINVAL;
	args->stream_id = 0;
	args->mode_generation = 0;

	crtc = drm_crtc_find(dev, file_priv, args->crtc_id);
	if (!crtc)
		return -ENOENT;
	output = drm_crtc_to_castkms_output(crtc);

	stream = kzalloc_obj(*stream);
	if (!stream)
		return -ENOMEM;
	stream->output = output;
	xa_init_flags(&stream->buffers, XA_FLAGS_ALLOC);

	mutex_lock(&capture_file->lock);
	mutex_lock(&output->capture.lock);
	if (output->capture.stream) {
		ret = -EBUSY;
		goto out_unlock_output;
	}

	ret = xa_alloc(&capture_file->streams, &stream_id, stream,
		       XA_LIMIT(1, INT_MAX), GFP_KERNEL);
	if (ret)
		goto out_unlock_output;

	stream->id = stream_id;
	castkms_capture_stream_snapshot_mode(stream);
	output->capture.stream = stream;
	args->stream_id = stream_id;
	args->mode_generation = stream->mode_generation;

out_unlock_output:
	mutex_unlock(&output->capture.lock);
	mutex_unlock(&capture_file->lock);
	if (ret) {
		xa_destroy(&stream->buffers);
		kfree(stream);
	}

	return ret;
}

int castkms_capture_stop_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file_priv)
{
	struct drm_castkms_capture_stop *args = data;
	struct castkms_capture_file *capture_file = file_priv->driver_priv;
	struct castkms_capture_stream *stream;

	if (args->flags || args->reserved)
		return -EINVAL;

	mutex_lock(&capture_file->lock);
	stream = xa_load(&capture_file->streams, args->stream_id);
	if (!stream) {
		mutex_unlock(&capture_file->lock);
		return -ENOENT;
	}

	xa_erase(&capture_file->streams, stream->id);
	castkms_capture_stream_destroy(stream);
	mutex_unlock(&capture_file->lock);

	return 0;
}

int castkms_capture_register_buffer_ioctl(struct drm_device *dev, void *data,
					  struct drm_file *file_priv)
{
	struct drm_castkms_capture_register_buffer *args = data;
	struct castkms_capture_file *capture_file = file_priv->driver_priv;
	struct castkms_capture_buffer *buffer = NULL;
	struct castkms_capture_stream *stream;
	struct drm_framebuffer *fb;
	u32 buffer_id;
	int ret;

	args->buffer_id = 0;
	if (args->flags != DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC)
		return -EINVAL;
	if (args->ready_syncobj_handle || args->reuse_syncobj_handle)
		return -EINVAL;

	mutex_lock(&capture_file->lock);
	stream = xa_load(&capture_file->streams, args->stream_id);
	if (!stream) {
		ret = -ENOENT;
		goto out_unlock;
	}
	if (drm_crtc_find(dev, file_priv, stream->output->crtc.base.id) !=
	    &stream->output->crtc) {
		ret = -ENOENT;
		goto out_unlock;
	}

	ret = castkms_capture_validate_mode(stream, args->mode_generation);
	if (ret)
		goto out_unlock;
	if (stream->num_buffers >= CASTKMS_CAPTURE_MAX_BUFFERS) {
		ret = -ENOSPC;
		goto out_unlock;
	}

	fb = drm_framebuffer_lookup(dev, file_priv, args->fb_id);
	if (!fb) {
		ret = -ENOENT;
		goto out_unlock;
	}
	if (!castkms_capture_fb_is_compatible(stream, fb)) {
		ret = -EINVAL;
		goto out_put_framebuffer;
	}
	if (!castkms_capture_fb_is_local(fb)) {
		ret = -EOPNOTSUPP;
		goto out_put_framebuffer;
	}

	buffer = kzalloc_obj(*buffer);
	if (!buffer) {
		ret = -ENOMEM;
		goto out_put_framebuffer;
	}
	init_completion(&buffer->delivery_done);
	complete_all(&buffer->delivery_done);

	ret = castkms_output_buffer_init(&buffer->output, fb);
	if (ret)
		goto out_destroy_buffer;

	ret = castkms_capture_validate_mode(stream, args->mode_generation);
	if (ret)
		goto out_destroy_buffer;

	ret = xa_alloc(&stream->buffers, &buffer_id, buffer,
		       XA_LIMIT(1, CASTKMS_CAPTURE_MAX_BUFFERS), GFP_KERNEL);
	if (ret)
		goto out_destroy_buffer;

	buffer->stream = stream;
	buffer->id = buffer_id;
	buffer->mode_generation = stream->mode_generation;
	buffer->sync_mode = args->flags;
	stream->num_buffers++;
	args->buffer_id = buffer_id;
	ret = 0;
	goto out_put_framebuffer;

out_destroy_buffer:
	castkms_capture_buffer_destroy(buffer);
out_put_framebuffer:
	drm_framebuffer_put(fb);
out_unlock:
	mutex_unlock(&capture_file->lock);
	return ret;
}

int castkms_capture_unregister_buffer_ioctl(struct drm_device *dev, void *data,
					    struct drm_file *file_priv)
{
	struct drm_castkms_capture_unregister_buffer *args = data;
	struct castkms_capture_file *capture_file = file_priv->driver_priv;
	struct castkms_capture_buffer *buffer;
	struct castkms_capture_output *capture;
	struct castkms_capture_stream *stream;
	unsigned long flags;
	bool busy;

	if (args->flags || args->reserved)
		return -EINVAL;

	mutex_lock(&capture_file->lock);
	stream = xa_load(&capture_file->streams, args->stream_id);
	if (!stream) {
		mutex_unlock(&capture_file->lock);
		return -ENOENT;
	}

	buffer = xa_load(&stream->buffers, args->buffer_id);
	if (!buffer) {
		mutex_unlock(&capture_file->lock);
		return -ENOENT;
	}

	capture = &stream->output->capture;
	spin_lock_irqsave(&stream->output->lock, flags);
	spin_lock(&capture->state_lock);
	busy = buffer->state != CASTKMS_CAPTURE_BUFFER_IDLE;
	spin_unlock(&capture->state_lock);
	spin_unlock_irqrestore(&stream->output->lock, flags);
	if (busy) {
		mutex_unlock(&capture_file->lock);
		return -EBUSY;
	}

	xa_erase(&stream->buffers, buffer->id);
	stream->num_buffers--;
	castkms_capture_buffer_destroy(buffer);
	mutex_unlock(&capture_file->lock);

	return 0;
}

int castkms_capture_queue_buffer_ioctl(struct drm_device *dev, void *data,
				       struct drm_file *file_priv)
{
	struct drm_castkms_capture_queue_buffer *args = data;
	struct castkms_capture_file *capture_file = file_priv->driver_priv;
	struct castkms_capture_completion failed_completion = {};
	struct castkms_capture_pending_event *pending;
	struct castkms_capture_buffer *buffer;
	struct castkms_capture_output *capture;
	struct castkms_capture_stream *stream;
	struct dma_fence *completion_fence = NULL;
	struct dma_fence *dependency = NULL;
	unsigned long flags;
	bool remove_callback = false;
	int ret;

	if (args->reserved ||
	    args->flags != DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC)
		return -EINVAL;
	if (args->flags == DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC &&
	    (args->ready_point || args->reuse_point))
		return -EINVAL;

	mutex_lock(&capture_file->lock);
	stream = xa_load(&capture_file->streams, args->stream_id);
	if (!stream) {
		ret = -ENOENT;
		goto out_unlock;
	}
	if (drm_crtc_find(dev, file_priv, stream->output->crtc.base.id) !=
	    &stream->output->crtc) {
		ret = -ENOENT;
		goto out_unlock;
	}

	ret = castkms_capture_validate_mode(stream, args->mode_generation);
	if (ret)
		goto out_unlock;

	buffer = xa_load(&stream->buffers, args->buffer_id);
	if (!buffer) {
		ret = -ENOENT;
		goto out_unlock;
	}
	if (buffer->sync_mode != DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC) {
		ret = -EINVAL;
		goto out_unlock;
	}

	capture = &stream->output->capture;
	spin_lock_irqsave(&capture->state_lock, flags);
	if (buffer->state != CASTKMS_CAPTURE_BUFFER_IDLE ||
	    capture->queued_buffer)
		ret = -EBUSY;
	else
		ret = 0;
	spin_unlock_irqrestore(&capture->state_lock, flags);
	if (ret)
		goto out_unlock;

	pending = kzalloc_obj(*pending);
	if (!pending) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	pending->event.base.type = DRM_CASTKMS_CAPTURE_EVENT_FRAME;
	pending->event.base.length = sizeof(pending->event);
	ret = drm_event_reserve_init(dev, file_priv, &pending->pending,
				     &pending->event.base);
	if (ret)
		goto out_free_event;

	if (args->flags == DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC)
		ret = castkms_capture_prepare_implicit_sync(buffer, &dependency,
							    &completion_fence);
	if (ret)
		goto out_cancel_event;

	ret = castkms_composer_get(stream->output,
				   CASTKMS_COMPOSER_CLIENT_CAPTURE);
	if (ret)
		goto out_signal_fence;


	/*
	 * Arm an existing dependency before publishing the buffer to vblank or
	 * mode-change cancellation. The callback may transition this private
	 * buffer to QUEUED before publication, but cannot make it globally
	 * visible on its own.
	 */
	spin_lock_irqsave(&capture->state_lock, flags);
	buffer->pending_event = pending;
	buffer->reuse_fence = dependency;
	buffer->completion_fence = completion_fence;
	buffer->user_data = args->user_data;
	buffer->dropped_frames = 0;
	reinit_completion(&buffer->delivery_done);
	buffer->reuse_callback_armed = !!dependency;
	buffer->state = dependency ?
		CASTKMS_CAPTURE_BUFFER_WAITING_REUSE :
		CASTKMS_CAPTURE_BUFFER_QUEUED;
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
				buffer->state =
					CASTKMS_CAPTURE_BUFFER_QUEUED;
			}
			spin_unlock_irqrestore(&capture->state_lock, flags);
			ret = 0;
		} else if (ret) {
			spin_lock_irqsave(&capture->state_lock, flags);
			buffer->reuse_callback_armed = false;
			castkms_capture_finish_locked(buffer, &failed_completion,
						      ret, 0,
						      capture->mode_generation, 0,
						      ktime_get());
			spin_unlock_irqrestore(&capture->state_lock, flags);
		}
	}
	if (ret)
		goto out_put_composer;

	/*
	 * Publish only after callback setup is complete. output->lock closes the
	 * final validation race with modesets and vblank selection.
	 */
	spin_lock_irqsave(&stream->output->lock, flags);
	spin_lock(&capture->state_lock);
	if (capture->mode_generation != stream->mode_generation) {
		ret = -ESTALE;
	} else if (!capture->active) {
		ret = -ENOLINK;
	} else if (capture->queued_buffer) {
		ret = -EBUSY;
	} else {
		capture->queued_buffer = buffer;
		ret = 0;
	}
	if (ret) {
		remove_callback = buffer->reuse_callback_armed;
		buffer->reuse_callback_armed = false;
		castkms_capture_finish_locked(buffer, &failed_completion, ret, 0,
					      capture->mode_generation, 0,
					      ktime_get());
	}
	spin_unlock(&capture->state_lock);
	spin_unlock_irqrestore(&stream->output->lock, flags);
	if (!ret)
		goto out_unlock;

	if (remove_callback)
		dma_fence_remove_callback(failed_completion.dependency,
					  &buffer->reuse_cb);
out_put_composer:
	castkms_composer_put(stream->output,
			     CASTKMS_COMPOSER_CLIENT_CAPTURE);
	if (failed_completion.event) {
		castkms_capture_cancel_completion(stream->output,
						  &failed_completion);
		goto out_unlock;
	}
out_signal_fence:
	castkms_capture_signal_fence(completion_fence, ret);
	dma_fence_put(dependency);
out_cancel_event:
	drm_event_cancel_free(dev, &pending->pending);
	goto out_unlock;
out_free_event:
	kfree(pending);
out_unlock:
	mutex_unlock(&capture_file->lock);
	return ret;
}
