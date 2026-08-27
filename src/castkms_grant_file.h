/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CASTKMS_GRANT_FILE_H_
#define _CASTKMS_GRANT_FILE_H_

#include <linux/types.h>

struct drm_device;
struct file;

struct file *castkms_grant_file_create(struct drm_device *dev,
				       unsigned int flags);
bool castkms_grant_file_release(struct file *file);

#endif /* _CASTKMS_GRANT_FILE_H_ */
