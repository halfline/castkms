// SPDX-License-Identifier: GPL-2.0-only

#include <linux/slab.h>

#include <drm/drm_auth.h>
#include <drm/drm_file.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_framebuffer_helper.h>

#include "castkms_framebuffer.h"

/**
 * struct castkms_framebuffer - GEM framebuffer with durable content identity
 * @base: DRM framebuffer
 * @capture_owner: Refcounted DRM master current when this framebuffer was
 *                 created, or NULL
 *
 * A framebuffer keeps the identity of the DRM ownership domain that supplied
 * it.  A later master cannot make residual pixels capture-safe merely by
 * issuing a no-op atomic commit which retains this framebuffer.
 */
struct castkms_framebuffer {
	struct drm_framebuffer base;
	struct drm_master *capture_owner;
};

#define to_castkms_framebuffer(fb) \
	container_of(fb, struct castkms_framebuffer, base)

static void castkms_framebuffer_destroy(struct drm_framebuffer *framebuffer)
{
	struct castkms_framebuffer *castkms_fb =
		to_castkms_framebuffer(framebuffer);

	if (castkms_fb->capture_owner)
		drm_master_put(&castkms_fb->capture_owner);
	drm_gem_fb_destroy(framebuffer);
}

static const struct drm_framebuffer_funcs castkms_framebuffer_funcs = {
	.destroy = castkms_framebuffer_destroy,
	.create_handle = drm_gem_fb_create_handle,
};

struct drm_framebuffer *
castkms_framebuffer_create(struct drm_device *dev, struct drm_file *file_priv,
			   const struct drm_format_info *info,
			   const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct castkms_framebuffer *castkms_fb;
	int ret;

	castkms_fb = kzalloc_obj(*castkms_fb);
	if (!castkms_fb)
		return ERR_PTR(-ENOMEM);

	/*
	 * A non-master primary client is associated with the current master's
	 * drm_master too.  Treating that association as ownership would let a
	 * future master create a framebuffer under A, present its own pixels as B,
	 * and leave those pixels mislabeled as A-owned.  Only the file which is
	 * current master at creation may establish a capture owner.
	 */
	if (file_priv && drm_is_current_master(file_priv))
		castkms_fb->capture_owner = drm_file_get_master(file_priv);

	ret = drm_gem_fb_init_with_funcs(dev, &castkms_fb->base, file_priv,
					 info, mode_cmd,
					 &castkms_framebuffer_funcs);
	if (ret) {
		if (castkms_fb->capture_owner)
			drm_master_put(&castkms_fb->capture_owner);
		kfree(castkms_fb);
		return ERR_PTR(ret);
	}

	return &castkms_fb->base;
}

struct drm_master *
castkms_framebuffer_capture_owner(const struct drm_framebuffer *framebuffer)
{
	if (!framebuffer || framebuffer->funcs != &castkms_framebuffer_funcs)
		return NULL;

	return to_castkms_framebuffer(framebuffer)->capture_owner;
}
