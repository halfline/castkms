// SPDX-License-Identifier: GPL-2.0-only

#include <linux/build_bug.h>
#include <linux/completion.h>
#include <linux/dma-fence.h>
#include <linux/dma-fence-chain.h>
#include <linux/dma-fence-unwrap.h>
#include <linux/kernel.h>
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

static const struct drm_castkms_capture_format castkms_capture_formats[] = {
	{
		.format = DRM_FORMAT_XRGB8888,
		.modifier = DRM_FORMAT_MOD_LINEAR,
	},
};

static_assert(sizeof(struct drm_castkms_capture_format) == 16);
static_assert(sizeof(struct drm_castkms_capture_query_caps) == 40);
static_assert(sizeof(struct drm_castkms_capture_start) == 24);


static void castkms_capture_stream_cancel(struct castkms_capture_stream *stream)
{
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

	castkms_capture_stream_cancel(stream);
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
	output->capture.mode_generation = 1;
	output->capture.width = 0;
	output->capture.height = 0;
	output->capture.active = false;

	return 0;
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

