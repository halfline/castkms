// SPDX-License-Identifier: GPL-2.0-only

#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include <drm/drm_crtc.h>
#include <drm/drm_managed.h>

#include <kunit/visibility.h>

#include "castkms_capture.h"
#include "castkms_capture_authority.h"
#include "castkms_capture_internal.h"
#include "castkms_connector.h"
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
	output->capture.mode_generation = 1;
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
	castkms_capture_stream_snapshot_mode(stream);

	if (mode_generation)
		*mode_generation = stream->mode_generation;

	return stream;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_stream_create);

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
	(void)stream;
	(void)status;
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
	castkms_capture_stream_cancel(stream, status);

	castkms_capture_stream_detach(stream);
	castkms_capture_authority_put(stream->authority);
	kfree(stream);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_stream_destroy);

void castkms_capture_mode_changed(struct castkms_output *output,
				  const struct drm_crtc_state *state)
{
	struct castkms_capture_output *capture = &output->capture;
	unsigned long flags;

	spin_lock_irqsave(&capture->state_lock, flags);
	capture->mode_generation++;
	capture->active = state->active;
	spin_unlock_irqrestore(&capture->state_lock, flags);
}
