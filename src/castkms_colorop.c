// SPDX-License-Identifier: GPL-2.0+

#include <linux/slab.h>
#include <drm/drm_colorop.h>
#include <drm/drm_print.h>
#include <drm/drm_property.h>
#include <drm/drm_plane.h>

#include "castkms_drv.h"

static const u64 supported_tfs =
	BIT(DRM_COLOROP_1D_CURVE_SRGB_EOTF) |
	BIT(DRM_COLOROP_1D_CURVE_SRGB_INV_EOTF);

static const struct drm_colorop_funcs castkms_colorop_funcs = {
	.destroy = drm_colorop_destroy,
};

#define CASTKMS_COLOR_PIPELINE_OPS 4

static int castkms_colorop_init(struct drm_plane *plane,
			       struct drm_colorop *colorop, unsigned int index)
{
	struct drm_device *dev = plane->dev;

	switch (index) {
	case 0:
	case 3:
		return drm_plane_colorop_curve_1d_init(dev, colorop, plane,
						      &castkms_colorop_funcs,
						      supported_tfs,
						      DRM_COLOROP_FLAG_ALLOW_BYPASS);
	case 1:
	case 2:
		return drm_plane_colorop_ctm_3x4_init(dev, colorop, plane,
						     &castkms_colorop_funcs,
						     DRM_COLOROP_FLAG_ALLOW_BYPASS);
	default:
		return -EINVAL;
	}
}

int castkms_initialize_colorops(struct drm_plane *plane)
{
	struct drm_prop_enum_list pipeline = {};
	struct drm_colorop *ops[CASTKMS_COLOR_PIPELINE_OPS] = {};
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(ops); i++) {
		ops[i] = kzalloc_obj(*ops[i]);
		if (!ops[i]) {
			drm_err(plane->dev, "failed to allocate colorop\n");
			ret = -ENOMEM;
			goto err_colorops;
		}

		ret = castkms_colorop_init(plane, ops[i], i);
		if (ret) {
			if (ops[i]->dev)
				drm_colorop_destroy(ops[i]);
			else
				kfree(ops[i]);
			ops[i] = NULL;
			goto err_colorops;
		}

		if (i)
			drm_colorop_set_next_property(ops[i - 1], ops[i]);
	}

	pipeline.type = ops[0]->base.id;
	pipeline.name = kasprintf(GFP_KERNEL, "Color Pipeline %u", ops[0]->base.id);
	if (!pipeline.name) {
		ret = -ENOMEM;
		goto err_colorops;
	}

	ret = drm_plane_create_color_pipeline_property(plane, &pipeline, 1);
	kfree(pipeline.name);
	if (ret)
		goto err_colorops;

	return 0;

err_colorops:
	for (i = 0; i < ARRAY_SIZE(ops); i++)
		if (ops[i])
			drm_colorop_destroy(ops[i]);

	return ret;
}
