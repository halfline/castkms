// SPDX-License-Identifier: GPL-2.0+

#include "castkms_config.h"
#include <linux/iosys-map.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_print.h>

#include "castkms_drv.h"
#include "castkms_formats.h"

static struct castkms_plane_state *castkms_plane_state_alloc(void)
{
	struct castkms_plane_state *castkms_state;

	castkms_state = kzalloc_obj(*castkms_state);
	if (!castkms_state)
		return NULL;

	castkms_state->frame.frame_info =
		kzalloc_obj(*castkms_state->frame.frame_info);
	if (!castkms_state->frame.frame_info) {
		kfree(castkms_state);
		return NULL;
	}

	return castkms_state;
}

static struct drm_plane_state *
castkms_plane_duplicate_state(struct drm_plane *plane)
{
	struct castkms_plane_state *castkms_state;

	castkms_state = castkms_plane_state_alloc();
	if (!castkms_state)
		return NULL;

	__drm_gem_duplicate_shadow_plane_state(plane, &castkms_state->base);

	return &castkms_state->base.base;
}

static void castkms_plane_destroy_state(struct drm_plane *plane,
				     struct drm_plane_state *old_state)
{
	struct castkms_plane_state *castkms_state = to_castkms_plane_state(old_state);

	if (castkms_state->frame.frame_info &&
	    castkms_state->frame.frame_info->fb) {
		/* dropping the reference we acquired in
		 * castkms_plane_atomic_update()
		 */
		drm_framebuffer_put(castkms_state->frame.frame_info->fb);
	}

	kfree(castkms_state->frame.colorops);
	kfree(castkms_state->frame.frame_info);
	castkms_state->frame.frame_info = NULL;

	__drm_gem_destroy_shadow_plane_state(&castkms_state->base);
	kfree(castkms_state);
}

int castkms_plane_snapshot_colorops(struct castkms_plane_state *plane_state,
				    struct drm_atomic_commit *state)
{
	struct drm_plane_state *base = &plane_state->base.base;
	struct castkms_colorop_snapshot *snapshots;
	struct drm_colorop *colorop;
	size_t count = 0;
	size_t i = 0;
	int ret;

	kfree(plane_state->frame.colorops);
	plane_state->frame.colorops = NULL;
	plane_state->frame.num_colorops = 0;

	if (!base->color_pipeline)
		return 0;

	ret = drm_atomic_add_affected_colorops(state, base->plane);
	if (ret)
		return ret;

	for (colorop = base->color_pipeline; colorop; colorop = colorop->next)
		count++;

	snapshots = kcalloc(count, sizeof(*snapshots), GFP_KERNEL);
	if (!snapshots)
		return -ENOMEM;

	for (colorop = base->color_pipeline; colorop; colorop = colorop->next) {
		struct drm_colorop_state *colorop_state;

		colorop_state = drm_atomic_get_new_colorop_state(state, colorop);
		if (WARN_ON(!colorop_state)) {
			ret = -EINVAL;
			goto err_free_snapshots;
		}

		ret = castkms_colorop_snapshot_init(&snapshots[i], colorop,
						    colorop_state);
		if (ret)
			goto err_free_snapshots;
		i++;
	}

	plane_state->frame.colorops = snapshots;
	plane_state->frame.num_colorops = count;

	return 0;

err_free_snapshots:
	kfree(snapshots);
	return ret;
}

static void castkms_plane_reset(struct drm_plane *plane)
{
	struct castkms_plane_state *castkms_state;

	if (plane->state) {
		castkms_plane_destroy_state(plane, plane->state);
		plane->state = NULL; /* must be set to NULL here */
	}

	castkms_state = castkms_plane_state_alloc();
	if (!castkms_state) {
		DRM_ERROR("Cannot allocate castkms_plane_state\n");
		return;
	}

	__drm_gem_reset_shadow_plane(plane, &castkms_state->base);
}

static const struct drm_plane_funcs castkms_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.reset			= castkms_plane_reset,
	.atomic_duplicate_state = castkms_plane_duplicate_state,
	.atomic_destroy_state	= castkms_plane_destroy_state,
};

static void castkms_plane_atomic_update(struct drm_plane *plane,
				     struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state,
									   plane);
	struct castkms_plane_state *castkms_plane_state;
	struct drm_shadow_plane_state *shadow_plane_state;
	struct drm_framebuffer *fb = new_state->fb;
	struct castkms_frame_info *frame_info;
	u32 fmt;

	if (!new_state->crtc || !fb)
		return;

	fmt = fb->format->format;
	castkms_plane_state = to_castkms_plane_state(new_state);
	shadow_plane_state = &castkms_plane_state->base;

	frame_info = castkms_plane_state->frame.frame_info;
	memcpy(&frame_info->src, &new_state->src, sizeof(struct drm_rect));
	memcpy(&frame_info->dst, &new_state->dst, sizeof(struct drm_rect));
	frame_info->fb = fb;
	frame_info->map = shadow_plane_state->map;
	drm_framebuffer_get(frame_info->fb);
	frame_info->rotation = new_state->rotation;

	castkms_plane_state->frame.pixel_read_line =
		castkms_get_pixel_read_line_function(fmt);
	castkms_get_conversion_matrix_to_argb_u16(fmt, new_state->color_encoding, new_state->color_range,
					  &castkms_plane_state->frame.conversion_matrix);
}

static int castkms_plane_atomic_check(struct drm_plane *plane,
				   struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state,
										 plane);
	struct drm_crtc_state *crtc_state;
	int ret;

	if (!new_plane_state->fb || WARN_ON(!new_plane_state->crtc))
		return 0;

	if (!castkms_get_pixel_read_line_function(new_plane_state->fb->format->format))
		return -EINVAL;
	if (!castkms_framebuffer_read_strides_are_valid(new_plane_state->fb))
		return -EINVAL;

	crtc_state = drm_atomic_get_crtc_state(state,
					       new_plane_state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	ret = drm_atomic_helper_check_plane_state(new_plane_state, crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  true, true);
	if (ret != 0)
		return ret;

	return 0;
}

static int castkms_prepare_fb(struct drm_plane *plane,
			   struct drm_plane_state *state)
{
	struct drm_shadow_plane_state *shadow_plane_state;
	struct drm_framebuffer *fb = state->fb;
	int ret;

	if (!fb)
		return 0;

	shadow_plane_state = to_drm_shadow_plane_state(state);

	ret = drm_gem_plane_helper_prepare_fb(plane, state);
	if (ret)
		return ret;

	ret = drm_gem_fb_vmap(fb, shadow_plane_state->map, NULL);
	if (ret)
		return ret;

	if (!castkms_framebuffer_maps_are_accessible(fb, shadow_plane_state->map)) {
		drm_gem_fb_vunmap(fb, shadow_plane_state->map);
		return -EOPNOTSUPP;
	}

	return 0;
}

static void castkms_cleanup_fb(struct drm_plane *plane,
			    struct drm_plane_state *state)
{
	struct drm_shadow_plane_state *shadow_plane_state;
	struct drm_framebuffer *fb = state->fb;

	if (!fb)
		return;

	shadow_plane_state = to_drm_shadow_plane_state(state);

	drm_gem_fb_vunmap(fb, shadow_plane_state->map);
}

static const struct drm_plane_helper_funcs castkms_plane_helper_funcs = {
	.atomic_update		= castkms_plane_atomic_update,
	.atomic_check		= castkms_plane_atomic_check,
	.prepare_fb		= castkms_prepare_fb,
	.cleanup_fb		= castkms_cleanup_fb,
};

struct castkms_plane *castkms_plane_init(struct castkms_device *castkmsdev,
				   struct castkms_config_plane *plane_cfg)
{
	struct drm_device *dev = &castkmsdev->drm;
	struct castkms_plane *plane;
	u32 *formats;
	int num_formats;
	unsigned int max_zpos = CASTKMS_MAX_OUTPUT_OBJECTS;
	unsigned int max_overlay_zpos = max_zpos - 1;
	int ret;

	num_formats = castkms_plane_formats_alloc(&formats);
	if (num_formats < 0)
		return ERR_PTR(num_formats);
	if (castkms_config_plane_get_type(plane_cfg) == DRM_PLANE_TYPE_CURSOR) {
		/* Cursor bitmap capture exports this byte layout as ARGB8888. */
		formats[0] = DRM_FORMAT_ARGB8888;
		num_formats = 1;
	}

	plane = drmm_universal_plane_alloc(dev, struct castkms_plane, base, 0,
					   &castkms_plane_funcs,
					   formats, num_formats,
					   NULL, castkms_config_plane_get_type(plane_cfg),
					   NULL);
	kfree(formats);
	if (IS_ERR(plane))
		return plane;

	drm_plane_helper_add(&plane->base, &castkms_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(&plane->base);

	ret = drm_plane_create_rotation_property(&plane->base, DRM_MODE_ROTATE_0,
					 DRM_MODE_ROTATE_MASK | DRM_MODE_REFLECT_MASK);
	if (ret)
		return ERR_PTR(ret);

	switch (castkms_config_plane_get_type(plane_cfg)) {
	case DRM_PLANE_TYPE_PRIMARY:
		ret = drm_plane_create_zpos_immutable_property(&plane->base, 0);
		break;
	case DRM_PLANE_TYPE_CURSOR:
		ret = drm_plane_create_zpos_immutable_property(&plane->base, max_zpos);
		break;
	case DRM_PLANE_TYPE_OVERLAY:
		ret = drm_plane_create_zpos_property(&plane->base, 1, 1, max_overlay_zpos);
		break;
	default:
		return ERR_PTR(-EINVAL);
	}
	if (ret)
		return ERR_PTR(ret);

	ret = drm_plane_create_color_properties(&plane->base,
					BIT(DRM_COLOR_YCBCR_BT601) |
					BIT(DRM_COLOR_YCBCR_BT709) |
					BIT(DRM_COLOR_YCBCR_BT2020),
					BIT(DRM_COLOR_YCBCR_LIMITED_RANGE) |
					BIT(DRM_COLOR_YCBCR_FULL_RANGE),
					DRM_COLOR_YCBCR_BT601,
					DRM_COLOR_YCBCR_FULL_RANGE);
	if (ret)
		return ERR_PTR(ret);

	if (castkms_config_plane_get_default_pipeline(plane_cfg)) {
		ret = castkms_initialize_colorops(&plane->base);
		if (ret)
			return ERR_PTR(ret);
	}

	return plane;
}
