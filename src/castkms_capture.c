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

static const struct drm_castkms_capture_format castkms_capture_formats[] = {
	{
		.format = DRM_FORMAT_XRGB8888,
		.modifier = DRM_FORMAT_MOD_LINEAR,
	},
};

static_assert(sizeof(struct drm_castkms_capture_format) == 16);
static_assert(sizeof(struct drm_castkms_capture_query_caps) == 40);

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
	xa_destroy(&capture_file->streams);
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

