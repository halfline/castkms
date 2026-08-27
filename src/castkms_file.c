// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bug.h>
#include <linux/fs.h>
#include <linux/slab.h>

#include <drm/drm_file.h>

#include "castkms_capture_owner.h"
#include "castkms_file.h"
#include "castkms_grant.h"
#include "castkms_grant_file.h"

int castkms_file_open(struct drm_device *dev, struct drm_file *file_priv)
{
	struct castkms_file *file_state;

	(void)dev;
	file_state = kzalloc_obj(*file_state);
	if (!file_state)
		return -ENOMEM;

	mutex_init(&file_state->capture_lock);
	xa_init_flags(&file_state->capture_streams, XA_FLAGS_ALLOC);
	xa_init_flags(&file_state->revocable_grants, XA_FLAGS_ALLOC);
	file_priv->driver_priv = file_state;

	return 0;
}

void castkms_file_postclose(struct drm_device *dev,
			    struct drm_file *file_priv)
{
	struct castkms_file *file_state = file_priv->driver_priv;

	castkms_capture_owner_file_close(dev, file_priv);
	if (WARN_ON(!file_state))
		return;
	castkms_grant_uapi_file_fini(dev, file_priv);

	xa_destroy(&file_state->capture_streams);
	xa_destroy(&file_state->revocable_grants);
	mutex_destroy(&file_state->capture_lock);
	kfree(file_state);
	file_priv->driver_priv = NULL;
}

bool castkms_file_is_grant(struct file *file)
{
	struct drm_file *file_priv = file->private_data;
	struct castkms_file *file_state;

	if (!file_priv)
		return false;
	file_state = file_priv->driver_priv;
	return file_state && READ_ONCE(file_state->grant_client);
}

int castkms_file_release(struct inode *inode, struct file *file)
{
	if (castkms_grant_file_release(file))
		return 0;

	return drm_release(inode, file);
}
