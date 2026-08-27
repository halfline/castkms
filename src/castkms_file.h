/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CASTKMS_FILE_H_
#define _CASTKMS_FILE_H_

#include <linux/mutex.h>
#include <linux/xarray.h>

struct castkms_capture_grant;
struct drm_client_dev;
struct drm_device;
struct drm_file;
struct file;
struct inode;

/**
 * struct castkms_file - CastKMS state private to one DRM file
 * @capture_lock: Protects the file-local capture stream namespace
 * @capture_streams: File-local capture UAPI stream adapters
 * @revocable_grants: Grants this file may query or revoke and whose close
 * permanently revokes them
 * @holder_grant: Live grant-fd authority adapter carried by this file, or NULL
 * @grant_client: Unregistered DRM client identifying a never-master grant file
 */
struct castkms_file {
	struct mutex capture_lock; /* Protects @capture_streams. */
	struct xarray capture_streams;
	struct xarray revocable_grants;
	struct castkms_capture_grant *holder_grant;
	struct drm_client_dev *grant_client;
};

int castkms_file_open(struct drm_device *dev, struct drm_file *file_priv);
void castkms_file_postclose(struct drm_device *dev,
			    struct drm_file *file_priv);
int castkms_file_release(struct inode *inode, struct file *file);
bool castkms_file_is_grant(struct file *file);

#endif /* _CASTKMS_FILE_H_ */
