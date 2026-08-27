/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CASTKMS_FILE_H_
#define _CASTKMS_FILE_H_

struct castkms_capture_grant;
struct drm_client_dev;
struct drm_device;
struct drm_file;
struct file;
struct inode;

/**
 * struct castkms_file - CastKMS state private to one DRM file
 * @holder_grant: Grant-fd wrapper carried by this file, or NULL
 * @grant_client: Unregistered DRM client backing a never-master grant fd
 */
struct castkms_file {
	struct castkms_capture_grant *holder_grant;
	struct drm_client_dev *grant_client;
};

int castkms_file_open(struct drm_device *dev, struct drm_file *file_priv);
void castkms_file_postclose(struct drm_device *dev,
			    struct drm_file *file_priv);
int castkms_file_release(struct inode *inode, struct file *file);
bool castkms_file_is_grant(struct file *file);

#endif /* _CASTKMS_FILE_H_ */
