// SPDX-License-Identifier: GPL-2.0-only

#include <linux/build_bug.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>

#include <drm/castkms_drm.h>
#include <drm/drm_crtc.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>

#include "castkms_capture.h"
#include "castkms_capture_authority.h"
#include "castkms_capture_uapi.h"
#include "castkms_file.h"
#include "castkms_grant.h"
#include "castkms_limits.h"
#include "castkms_output.h"

struct castkms_capture_uapi_stream {
	struct kref ref;
	struct castkms_capture_authority_resource resource;
	struct castkms_capture_authority *authority;
	struct castkms_file *file_state;
	struct castkms_capture_stream *stream;
	u32 stream_id;
	bool published;
};

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
static_assert(sizeof(struct drm_castkms_capture_start) == 24);

static void castkms_capture_uapi_stream_release(struct kref *ref)
{
	struct castkms_capture_uapi_stream *uapi_stream =
		container_of(ref, struct castkms_capture_uapi_stream, ref);

	WARN_ON(uapi_stream->published);
	WARN_ON(uapi_stream->resource.authority);
	WARN_ON(uapi_stream->stream);
	castkms_capture_authority_put(uapi_stream->authority);
	kfree(uapi_stream);
}

static void
castkms_capture_uapi_stream_get(struct castkms_capture_uapi_stream *uapi_stream)
{
	kref_get(&uapi_stream->ref);
}

static void
castkms_capture_uapi_stream_put(struct castkms_capture_uapi_stream *uapi_stream)
{
	kref_put(&uapi_stream->ref, castkms_capture_uapi_stream_release);
}

static bool castkms_capture_uapi_unpublish_stream(
	struct castkms_capture_uapi_stream *uapi_stream)
{
	struct castkms_file *file_state = uapi_stream->file_state;

	lockdep_assert_held(&file_state->capture_lock);
	if (!uapi_stream->published)
		return false;
	if (!WARN_ON(xa_load(&file_state->capture_streams,
			       uapi_stream->stream_id) != uapi_stream))
		xa_erase(&file_state->capture_streams, uapi_stream->stream_id);
	uapi_stream->published = false;
	return true;
}

static bool castkms_capture_uapi_stream_needs_cleanup(
	struct castkms_capture_authority_resource *resource,
	enum castkms_capture_authority_cleanup_reason reason, u64 generation)
{
	struct castkms_capture_uapi_stream *uapi_stream =
		container_of(resource, struct castkms_capture_uapi_stream,
			     resource);

	switch (reason) {
	case CASTKMS_CAPTURE_AUTHORITY_CLEANUP_MASTER_EPOCH:
		return castkms_capture_authority_generation_is_stale(
			castkms_capture_stream_authority_generation(
				uapi_stream->stream),
			generation);
	case CASTKMS_CAPTURE_AUTHORITY_CLEANUP_DISCONNECT:
		return true;
	default:
		return false;
	}
}

static void castkms_capture_uapi_stream_revoke(
	struct castkms_capture_authority_resource *resource, int status)
{
	struct castkms_capture_uapi_stream *uapi_stream =
		container_of(resource, struct castkms_capture_uapi_stream,
			     resource);
	struct castkms_capture_stream *stream = uapi_stream->stream;
	bool put_mapping;

	mutex_lock(&uapi_stream->file_state->capture_lock);
	put_mapping =
		castkms_capture_uapi_unpublish_stream(uapi_stream);
	mutex_unlock(&uapi_stream->file_state->capture_lock);

	uapi_stream->stream = NULL;
	castkms_capture_stream_destroy(stream, status);
	if (put_mapping)
		castkms_capture_uapi_stream_put(uapi_stream);
	/* Drop the resource-lifetime reference last. */
	castkms_capture_uapi_stream_put(uapi_stream);
}

static const struct castkms_capture_authority_resource_ops
castkms_capture_uapi_stream_resource_ops = {
	.needs_cleanup = castkms_capture_uapi_stream_needs_cleanup,
	.revoke = castkms_capture_uapi_stream_revoke,
};

static void castkms_capture_uapi_stream_stop(
	struct castkms_capture_uapi_stream *uapi_stream, int status)
{
	struct castkms_capture_stream *stream;

	if (!castkms_capture_authority_unregister_resource(
		    uapi_stream->authority, &uapi_stream->resource))
		return;

	stream = uapi_stream->stream;
	uapi_stream->stream = NULL;
	castkms_capture_stream_destroy(stream, status);
	castkms_capture_uapi_stream_put(uapi_stream);
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
	args->flags = DRM_CASTKMS_CAPTURE_CAP_GRANT_FD;
	args->max_registered_buffers = 0;
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
	struct castkms_file *file_state = file_priv->driver_priv;
	struct castkms_capture_uapi_stream *uapi_stream;
	struct castkms_capture_authority *authority;
	struct castkms_capture_stream *stream;
	struct castkms_output *output;
	struct drm_crtc *crtc;
	u32 rights = CASTKMS_CAPTURE_AUTHORITY_CAPTURE_PIXELS |
		     CASTKMS_CAPTURE_AUTHORITY_READ_CURSOR;
	u32 stream_id;
	bool put_mapping = false;
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
	ret = castkms_grant_begin_crtc(file_priv, crtc, rights, &authority);
	if (ret)
		return ret;

	stream = castkms_capture_stream_create(output, authority,
					       &args->mode_generation);
	if (IS_ERR(stream)) {
		ret = PTR_ERR(stream);
		goto out_end_grant;
	}
	uapi_stream = kzalloc_obj(*uapi_stream);
	if (!uapi_stream) {
		ret = -ENOMEM;
		goto out_destroy_stream;
	}
	kref_init(&uapi_stream->ref);
	uapi_stream->authority = authority;
	castkms_capture_authority_get(authority);
	uapi_stream->file_state = file_state;
	uapi_stream->stream = stream;
	ret = castkms_capture_authority_register_resource(
		authority, &uapi_stream->resource,
		&castkms_capture_uapi_stream_resource_ops);
	if (ret)
		goto out_destroy_unregistered_uapi_stream;

	ret = castkms_capture_stream_status(stream);
	if (ret)
		goto out_stop_uapi_stream;

	mutex_lock(&file_state->capture_lock);
	ret = xa_alloc(&file_state->capture_streams, &stream_id, uapi_stream,
		       XA_LIMIT(1, INT_MAX), GFP_KERNEL);
	if (ret)
		goto out_unlock;
	uapi_stream->stream_id = stream_id;
	uapi_stream->published = true;
	castkms_capture_uapi_stream_get(uapi_stream);
	ret = castkms_capture_stream_attach(stream);
	if (!ret)
		ret = castkms_capture_stream_status(stream);
	if (!ret)
		ret = castkms_capture_stream_validate_mode(
			stream, args->mode_generation);
	if (!ret) {
		ret = castkms_capture_stream_status(stream);
		if (ret == -EACCES)
			ret = -ESTALE;
	}
	if (ret) {
		put_mapping =
			castkms_capture_uapi_unpublish_stream(uapi_stream);
		goto out_unlock;
	}
	mutex_unlock(&file_state->capture_lock);

	args->stream_id = stream_id;
	castkms_grant_end(authority);
	return 0;

out_unlock:
	mutex_unlock(&file_state->capture_lock);
	if (put_mapping)
		castkms_capture_uapi_stream_put(uapi_stream);
out_stop_uapi_stream:
	castkms_capture_uapi_stream_stop(uapi_stream, -ECANCELED);
	goto out_clear_generation;
out_destroy_unregistered_uapi_stream:
	uapi_stream->stream = NULL;
	castkms_capture_stream_destroy(stream, -ECANCELED);
	castkms_capture_uapi_stream_put(uapi_stream);
	goto out_clear_generation;
out_destroy_stream:
	castkms_capture_stream_destroy(stream, -ECANCELED);
out_clear_generation:
	args->mode_generation = 0;
out_end_grant:
	castkms_grant_end(authority);
	return ret;
}
