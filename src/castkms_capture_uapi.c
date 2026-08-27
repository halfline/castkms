// SPDX-License-Identifier: GPL-2.0-only

#include <linux/build_bug.h>
#include <linux/kernel.h>
#include <linux/uaccess.h>

#include <drm/castkms_drm.h>
#include <drm/drm_crtc.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>

#include "castkms_capture_uapi.h"
#include "castkms_limits.h"

static const struct drm_castkms_capture_format castkms_capture_formats[] = {
	{
		.format = DRM_FORMAT_XRGB8888,
		.modifier = DRM_FORMAT_MOD_LINEAR,
	},
};

static_assert(sizeof(struct drm_castkms_capture_format) == 16);
static_assert(CASTKMS_MIN_WIDTH == DRM_CASTKMS_CAPTURE_MIN_WIDTH);
static_assert(CASTKMS_MIN_HEIGHT == DRM_CASTKMS_CAPTURE_MIN_HEIGHT);
static_assert(CASTKMS_MAX_WIDTH == DRM_CASTKMS_CAPTURE_MAX_WIDTH);
static_assert(CASTKMS_MAX_HEIGHT == DRM_CASTKMS_CAPTURE_MAX_HEIGHT);
static_assert(CASTKMS_MAX_CURSOR_WIDTH ==
	      DRM_CASTKMS_CAPTURE_MAX_CURSOR_WIDTH);
static_assert(CASTKMS_MAX_CURSOR_HEIGHT ==
	      DRM_CASTKMS_CAPTURE_MAX_CURSOR_HEIGHT);
static_assert(CASTKMS_MAX_EDID_SIZE == DRM_CASTKMS_CAPTURE_MAX_EDID_SIZE);
static_assert(sizeof(struct drm_castkms_capture_query_caps) == 40);

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
	args->flags = DRM_CASTKMS_CAPTURE_CAP_GRANT_FD;
	args->max_registered_buffers = 0;
	args->format_count = ARRAY_SIZE(castkms_capture_formats);

	if (capacity >= ARRAY_SIZE(castkms_capture_formats) &&
	    copy_to_user(formats, castkms_capture_formats,
			 sizeof(castkms_capture_formats)))
		return -EFAULT;

	return 0;
}
