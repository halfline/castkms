// SPDX-License-Identifier: GPL-2.0-only

#include "castkms_grant_file.h"

#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>

#include <drm/drm_client.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>

#include "castkms_file.h"

/*
 * drm_client_init() allocates a fresh primary-minor drm_file without calling
 * drm_master_open(). Keep the client unregistered and expose that file through
 * an anonymous fd. The holder gets independent GEM, syncobj, event, and
 * framebuffer namespaces without ever changing DRM master.
 */
struct file *castkms_grant_file_create(struct drm_device *dev,
				       unsigned int flags)
{
	struct castkms_file *file_state;
	struct drm_client_dev *client;
	struct drm_file *file_priv;
	struct file *file;
	int ret;

	client = kzalloc_obj(*client);
	if (!client)
		return ERR_PTR(-ENOMEM);

	ret = drm_client_init(dev, client, "castkms-grant", NULL);
	if (ret)
		goto out_free_client;

	file_priv = client->file;
	file_state = file_priv->driver_priv;
	if (WARN_ON(!file_state)) {
		ret = -EINVAL;
		goto out_release_client;
	}

	file = anon_inode_getfile("castkms-grant", dev->driver->fops,
				  file_priv, O_RDWR | flags);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto out_release_client;
	}

	file->f_mapping = dev->anon_inode->i_mapping;
	file_priv->filp = file;
	file_priv->authenticated = false;
	file_state->grant_client = client;

	return file;

out_release_client:
	drm_client_release(client);
out_free_client:
	kfree(client);
	return ERR_PTR(ret);
}

bool castkms_grant_file_release(struct file *file)
{
	struct drm_file *file_priv = file->private_data;
	struct drm_client_dev *client;
	struct castkms_file *file_state;

	if (!file_priv)
		return false;
	file_state = file_priv->driver_priv;
	if (!file_state || !file_state->grant_client)
		return false;

	client = file_state->grant_client;
	file_state->grant_client = NULL;
	drm_client_release(client);
	kfree(client);
	file->private_data = NULL;

	return true;
}
