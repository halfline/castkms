/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CASTKMS_GRANT_H_
#define _CASTKMS_GRANT_H_

#include <linux/types.h>

struct castkms_capture_authority;
struct castkms_capture_grant;
struct castkms_device;
struct drm_connector;
struct drm_crtc;
struct drm_device;
struct drm_file;
struct drm_master;
struct drm_printer;

/* Grant-fd UAPI entry points. */
int castkms_grant_create_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file_priv);
int castkms_grant_revoke_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file_priv);
int castkms_grant_get_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file_priv);

/* Translate a grant-bearing DRM file into a locked core authority. */
int castkms_grant_begin(struct drm_file *file_priv,
			struct drm_connector *connector, u32 rights,
			struct castkms_capture_authority **authority_out);
int castkms_grant_begin_crtc(
	struct drm_file *file_priv, struct drm_crtc *crtc, u32 rights,
	struct castkms_capture_authority **authority_out);
int castkms_grant_begin_owned(
	struct drm_file *file_priv,
	struct castkms_capture_authority *owned_authority, u32 rights,
	struct castkms_capture_authority **authority_out);
void castkms_grant_end(struct castkms_capture_authority *authority);

void castkms_grant_file_close(struct drm_device *dev,
			      struct drm_file *file_priv);
int castkms_grant_device_init(struct castkms_device *castkmsdev);
void castkms_grant_show_fdinfo(struct drm_printer *p, struct drm_file *file);

#if IS_ENABLED(CONFIG_KUNIT)
bool castkms_grant_master_is_owner(const struct drm_master *master);
int castkms_grant_creation_status(
	u32 flags, bool privileged, bool caller_current_master,
	bool caller_owner_master, bool current_owner_master);
#endif

#endif /* _CASTKMS_GRANT_H_ */
