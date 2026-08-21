/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CASTKMS_CAPTURE_UAPI_H_
#define _CASTKMS_CAPTURE_UAPI_H_

#include <linux/mutex.h>
#include <linux/xarray.h>

struct castkms_capture_authority;
struct castkms_capture_grant;
struct drm_device;
struct drm_file;

/**
 * struct castkms_capture_file - CastKMS UAPI state private to one DRM file
 * @lock: Protects file-local capture streams
 * @streams: File-local stream ID namespace
 * @revocable_grants: Grants that this file may query or revoke and whose
 * close permanently revokes them
 * @holder_grant: Grant-fd wrapper carried by this file, or NULL
 */
struct castkms_capture_file {
	struct mutex lock; /* Protects @streams. */
	struct xarray streams;
	struct xarray revocable_grants;
	struct castkms_capture_grant *holder_grant;
};

int castkms_capture_query_caps_ioctl(struct drm_device *dev, void *data,
				     struct drm_file *file_priv);
int castkms_capture_start_ioctl(struct drm_device *dev, void *data,
				struct drm_file *file_priv);
int castkms_capture_stop_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file_priv);
int castkms_capture_register_buffer_ioctl(struct drm_device *dev, void *data,
					  struct drm_file *file_priv);
int castkms_capture_unregister_buffer_ioctl(struct drm_device *dev, void *data,
					    struct drm_file *file_priv);
int castkms_capture_queue_buffer_ioctl(struct drm_device *dev, void *data,
				       struct drm_file *file_priv);
int castkms_capture_set_output_edid_ioctl(struct drm_device *dev, void *data,
					  struct drm_file *file_priv);
int castkms_capture_attach_monitor_ioctl(struct drm_device *dev, void *data,
					 struct drm_file *file_priv);
int castkms_capture_detach_monitor_ioctl(struct drm_device *dev, void *data,
					 struct drm_file *file_priv);
int castkms_capture_read_cursor_bitmap_ioctl(struct drm_device *dev, void *data,
					     struct drm_file *file_priv);

int castkms_capture_file_open(struct drm_device *dev,
			      struct drm_file *file_priv);
void castkms_capture_file_close(struct drm_device *dev,
				struct drm_file *file_priv);
void castkms_capture_stop_authority_streams(
	struct drm_file *file_priv,
	struct castkms_capture_authority *authority, int status);
void castkms_capture_stop_authority_streams_before(
	struct drm_file *file_priv,
	struct castkms_capture_authority *authority,
	u64 before_generation, int status);

#endif /* _CASTKMS_CAPTURE_UAPI_H_ */
