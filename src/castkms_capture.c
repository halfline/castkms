// SPDX-License-Identifier: GPL-2.0-only

#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include <drm/drm_crtc.h>
#include <drm/drm_managed.h>

#include <kunit/visibility.h>

#include "castkms_capture.h"
#include "castkms_capture_authority.h"
#include "castkms_capture_internal.h"
#include "castkms_connector.h"
#include "castkms_crtc.h"
#include "castkms_frame_dispatch.h"
#include "castkms_output.h"

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

int castkms_capture_stream_validate_mode(
	const struct castkms_capture_stream *stream, u64 mode_generation)
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

struct castkms_capture_stream *
castkms_capture_stream_create(struct castkms_output *output,
			      struct castkms_capture_authority *authority,
			      u64 *mode_generation)
{
	struct castkms_capture_stream *stream;

	stream = kzalloc_obj(*stream);
	if (!stream)
		return ERR_PTR(-ENOMEM);

	stream->output = output;
	stream->authority = authority;
	castkms_capture_authority_get(authority);
	stream->authority_generation =
		castkms_capture_authority_stream_generation(authority);
	INIT_LIST_HEAD(&stream->buffers);
	init_completion(&stream->deliveries_done);
	complete_all(&stream->deliveries_done);
	castkms_capture_stream_snapshot_mode(stream);

	if (mode_generation)
		*mode_generation = stream->mode_generation;

	return stream;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_stream_create);

struct castkms_output *
castkms_capture_stream_output(const struct castkms_capture_stream *stream)
{
	return stream->output;
}

bool castkms_capture_stream_has_authority(
	const struct castkms_capture_stream *stream,
	const struct castkms_capture_authority *authority)
{
	return stream->authority == authority;
}

u64 castkms_capture_stream_authority_generation(
	const struct castkms_capture_stream *stream)
{
	return stream->authority_generation;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_stream_authority_generation);

int castkms_capture_stream_status(
	const struct castkms_capture_stream *stream)
{
	return castkms_capture_authority_stream_status(
		stream->authority, stream->output, stream->authority_generation);
}

int castkms_capture_stream_attach(struct castkms_capture_stream *stream)
{
	struct castkms_capture_output *capture = &stream->output->capture;

	mutex_lock(&capture->lock);
	if (capture->stream) {
		mutex_unlock(&capture->lock);
		return -EBUSY;
	}
	capture->stream = stream;
	stream->attached = true;
	mutex_unlock(&capture->lock);

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_stream_attach);

static void castkms_capture_stream_cancel(struct castkms_capture_stream *stream,
					  int status)
{
	struct castkms_capture_output *capture = &stream->output->capture;
	struct castkms_capture_completion completion = {};
	struct castkms_capture_buffer *queued_buffer = NULL;
	struct castkms_capture_buffer *buffer;
	unsigned long flags;
	bool in_flight = false;
	bool remove_callback = false;
	bool put_dispatch = false;

	/*
	 * Serialize against vblank selection through output->lock. Once that
	 * lock is released, an in-flight buffer's work has been queued and can
	 * be flushed without racing a late queue_work().
	 */
	spin_lock_irqsave(&stream->output->lock, flags);
	spin_lock(&capture->state_lock);
	stream->cancel_status = status;
	buffer = capture->queued_buffer;
	if (buffer && buffer->stream == stream) {
		queued_buffer = buffer;
		capture->queued_buffer = NULL;
		remove_callback = buffer->reuse_callback_armed;
		buffer->reuse_callback_armed = false;
		castkms_capture_buffer_finish(
			buffer, &completion, status, status == -ECANCELED,
			false, capture->mode_generation, 0, ktime_get());
		put_dispatch = true;
	}

	buffer = capture->in_flight_buffer;
	if (buffer && buffer->stream == stream)
		in_flight = true;
	spin_unlock(&capture->state_lock);
	spin_unlock_irqrestore(&stream->output->lock, flags);

	if (remove_callback)
		castkms_capture_buffer_remove_reuse_callback(
			queued_buffer, completion.dependency);
	if (put_dispatch)
		castkms_frame_dispatch_put(
			stream->output, CASTKMS_FRAME_DISPATCH_CLIENT_CAPTURE);
	castkms_capture_deliver_completion(stream->output, &completion);
	if (in_flight) {
		flush_workqueue(stream->output->dispatch_workq);
		flush_workqueue(stream->output->capture_workq);
	}
	list_for_each_entry(buffer, &stream->buffers, link)
		wait_for_completion(&buffer->submit_done);
	wait_for_completion(&stream->deliveries_done);
}

static void castkms_capture_stream_detach(struct castkms_capture_stream *stream)
{
	struct castkms_capture_output *capture = &stream->output->capture;

	if (!stream->attached)
		return;

	mutex_lock(&capture->lock);
	if (WARN_ON(capture->stream != stream)) {
		mutex_unlock(&capture->lock);
		return;
	}
	capture->stream = NULL;
	stream->attached = false;
	mutex_unlock(&capture->lock);
}

void castkms_capture_stream_destroy(struct castkms_capture_stream *stream,
				    int status)
{
	struct castkms_capture_buffer *buffer, *next;

	castkms_capture_stream_cancel(stream, status);
	list_for_each_entry_safe(buffer, next, &stream->buffers, link) {
		list_del(&buffer->link);
		castkms_capture_buffer_destroy(buffer);
	}

	castkms_capture_stream_detach(stream);
	castkms_capture_authority_put(stream->authority);
	kfree(stream);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_stream_destroy);

bool castkms_capture_mode_changed(struct castkms_output *output,
				  const struct drm_crtc_state *state,
				  struct castkms_capture_completion *completion)
{
	struct castkms_capture_output *capture = &output->capture;
	struct castkms_capture_buffer *buffer;
	unsigned long flags;
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
		castkms_capture_buffer_finish(
			buffer, completion, -ESTALE, false, true,
			capture->mode_generation, 0, ktime_get());
		cancelled = true;
	}
	spin_unlock_irqrestore(&capture->state_lock, flags);

	if (remove_callback)
		castkms_capture_buffer_remove_reuse_callback(
			buffer, completion->dependency);

	return cancelled;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_mode_changed);

bool castkms_capture_prepare_frame(struct castkms_output *output,
				   struct castkms_crtc_state *state,
				   u64 sequence, ktime_t timestamp)
{
	struct castkms_capture_output *capture = &output->capture;
	struct castkms_capture_buffer *buffer;
	struct castkms_capture_authority *authority;
	unsigned long flags;
	u64 authority_generation;
	int authority_status;

	spin_lock_irqsave(&capture->state_lock, flags);
	buffer = capture->queued_buffer;
	if (!buffer) {
		spin_unlock_irqrestore(&capture->state_lock, flags);
		return false;
	}
	authority = buffer->stream->authority;
	authority_generation = buffer->stream->authority_generation;
	authority_status = castkms_capture_authority_evaluate_stream_status(
		authority, output, authority_generation);
	if (capture->in_flight_buffer ||
	    buffer->state != CASTKMS_CAPTURE_BUFFER_QUEUED ||
	    buffer->mode_generation != capture->mode_generation ||
	    !capture->active || authority_status) {
		if (buffer->dropped_frames != U32_MAX)
			buffer->dropped_frames++;
		spin_unlock_irqrestore(&capture->state_lock, flags);
		return false;
	}

	capture->queued_buffer = NULL;
	capture->in_flight_buffer = buffer;
	buffer->sequence = sequence;
	buffer->timestamp = timestamp;
	castkms_capture_buffer_set_state(
		buffer, CASTKMS_CAPTURE_BUFFER_IN_FLIGHT);
	spin_unlock_irqrestore(&capture->state_lock, flags);

	spin_lock_irqsave(&output->dispatch_lock, flags);
	if (WARN_ON(state->capture_pending || state->active_capture)) {
		spin_unlock_irqrestore(&output->dispatch_lock, flags);
		spin_lock_irqsave(&capture->state_lock, flags);
		capture->in_flight_buffer = NULL;
		capture->queued_buffer = buffer;
		castkms_capture_buffer_set_state(
			buffer, CASTKMS_CAPTURE_BUFFER_QUEUED);
		spin_unlock_irqrestore(&capture->state_lock, flags);
		return false;
	}
	state->active_capture = buffer;
	state->capture_pending = true;
	spin_unlock_irqrestore(&output->dispatch_lock, flags);

	return true;
}
