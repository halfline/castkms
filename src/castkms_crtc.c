// SPDX-License-Identifier: GPL-2.0+

#include <linux/dma-fence.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>
#include <drm/drm_vblank_helper.h>

#include "castkms_drv.h"

static bool castkms_crtc_handle_vblank_timeout(struct drm_crtc *crtc)
{
	struct castkms_output *output = drm_crtc_to_castkms_output(crtc);
	struct castkms_crtc_state *state;
	bool ret, fence_cookie;

	fence_cookie = dma_fence_begin_signalling();

	spin_lock(&output->lock);
	ret = drm_crtc_handle_vblank(crtc);
	if (!ret)
		DRM_ERROR("castkms failure on handling vblank");

	state = output->composer_state;
	spin_unlock(&output->lock);

	if (state && output->composer_enabled) {
		u64 frame = drm_crtc_accurate_vblank_count(crtc);

		/* update frame_start only if a queued castkms_composer_worker()
		 * has read the data
		 */
		spin_lock(&output->composer_lock);
		if (!state->crc_pending)
			state->frame_start = frame;
		else
			DRM_DEBUG_DRIVER("crc worker falling behind, frame_start: %llu, frame_end: %llu\n",
					 state->frame_start, frame);
		state->frame_end = frame;
		state->crc_pending = true;
		spin_unlock(&output->composer_lock);

		ret = queue_work(output->composer_workq, &state->composer_work);
		if (!ret)
			DRM_DEBUG_DRIVER("Composer worker already queued\n");
	}

	dma_fence_end_signalling(fence_cookie);

	return true;
}

static struct drm_crtc_state *
castkms_atomic_crtc_duplicate_state(struct drm_crtc *crtc)
{
	struct castkms_crtc_state *castkms_state;

	if (WARN_ON(!crtc->state))
		return NULL;

	castkms_state = kzalloc_obj(*castkms_state);
	if (!castkms_state)
		return NULL;

	__drm_atomic_helper_crtc_duplicate_state(crtc, &castkms_state->base);

	INIT_WORK(&castkms_state->composer_work, castkms_composer_worker);

	return &castkms_state->base;
}

static void castkms_atomic_crtc_destroy_state(struct drm_crtc *crtc,
					   struct drm_crtc_state *state)
{
	struct castkms_crtc_state *castkms_state = to_castkms_crtc_state(state);

	__drm_atomic_helper_crtc_destroy_state(state);

	WARN_ON(work_pending(&castkms_state->composer_work));
	kfree(castkms_state->active_planes);
	kfree(castkms_state);
}

static void castkms_atomic_crtc_reset(struct drm_crtc *crtc)
{
	struct castkms_crtc_state *castkms_state = kzalloc_obj(*castkms_state);

	if (crtc->state) {
		castkms_atomic_crtc_destroy_state(crtc, crtc->state);
		crtc->state = NULL;
	}

	if (!castkms_state)
		return;

	__drm_atomic_helper_crtc_reset(crtc, &castkms_state->base);
	INIT_WORK(&castkms_state->composer_work, castkms_composer_worker);
}

static const struct drm_crtc_funcs castkms_crtc_funcs = {
	.set_config             = drm_atomic_helper_set_config,
	.page_flip              = drm_atomic_helper_page_flip,
	.reset                  = castkms_atomic_crtc_reset,
	.atomic_duplicate_state = castkms_atomic_crtc_duplicate_state,
	.atomic_destroy_state   = castkms_atomic_crtc_destroy_state,
	DRM_CRTC_VBLANK_TIMER_FUNCS,
	.get_crc_sources	= castkms_get_crc_sources,
	.set_crc_source		= castkms_set_crc_source,
	.verify_crc_source	= castkms_verify_crc_source,
};

static int castkms_crtc_atomic_check(struct drm_crtc *crtc,
				  struct drm_atomic_commit *state)
{
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state,
									  crtc);
	struct castkms_crtc_state *castkms_state = to_castkms_crtc_state(crtc_state);
	struct drm_plane *plane;
	struct drm_plane_state *plane_state;
	int i = 0, ret;

	if (castkms_state->active_planes)
		return 0;

	ret = drm_atomic_add_affected_planes(crtc_state->state, crtc);
	if (ret < 0)
		return ret;

	drm_for_each_plane_mask(plane, crtc->dev, crtc_state->plane_mask) {
		plane_state = drm_atomic_get_new_plane_state(crtc_state->state, plane);
		WARN_ON(!plane_state);

		if (!plane_state->visible)
			continue;

		i++;
	}

	castkms_state->active_planes = kzalloc_objs(*castkms_state->active_planes, i);
	if (!castkms_state->active_planes)
		return -ENOMEM;
	castkms_state->num_active_planes = i;

	i = 0;
	drm_for_each_plane_mask(plane, crtc->dev, crtc_state->plane_mask) {
		plane_state = drm_atomic_get_new_plane_state(crtc_state->state, plane);

		if (!plane_state->visible)
			continue;

		castkms_state->active_planes[i++] =
			to_castkms_plane_state(plane_state);
	}

	return 0;
}

static void castkms_crtc_atomic_begin(struct drm_crtc *crtc,
				   struct drm_atomic_commit *state)
	__acquires(&castkms_output->lock)
{
	struct castkms_output *castkms_output = drm_crtc_to_castkms_output(crtc);

	/* This lock is held across the atomic commit to block vblank timer
	 * from scheduling castkms_composer_worker until the composer is updated
	 */
	spin_lock_irq(&castkms_output->lock);
}

static void castkms_crtc_atomic_flush(struct drm_crtc *crtc,
				   struct drm_atomic_commit *state)
	__releases(&castkms_output->lock)
{
	struct castkms_output *castkms_output = drm_crtc_to_castkms_output(crtc);

	if (crtc->state->event) {
		spin_lock(&crtc->dev->event_lock);

		if (drm_crtc_vblank_get(crtc) != 0)
			drm_crtc_send_vblank_event(crtc, crtc->state->event);
		else
			drm_crtc_arm_vblank_event(crtc, crtc->state->event);

		spin_unlock(&crtc->dev->event_lock);

		crtc->state->event = NULL;
	}

	castkms_output->composer_state = to_castkms_crtc_state(crtc->state);

	spin_unlock_irq(&castkms_output->lock);
}

static const struct drm_crtc_helper_funcs castkms_crtc_helper_funcs = {
	.atomic_check	= castkms_crtc_atomic_check,
	.atomic_begin	= castkms_crtc_atomic_begin,
	.atomic_flush	= castkms_crtc_atomic_flush,
	.atomic_enable	= drm_crtc_vblank_atomic_enable,
	.atomic_disable	= drm_crtc_vblank_atomic_disable,
	.handle_vblank_timeout = castkms_crtc_handle_vblank_timeout,
};

struct castkms_output *castkms_crtc_init(struct drm_device *dev, struct drm_plane *primary,
				   struct drm_plane *cursor)
{
	struct castkms_output *castkms_out;
	struct drm_crtc *crtc;
	int ret;

	castkms_out = drmm_crtc_alloc_with_planes(dev, struct castkms_output, crtc,
					       primary, cursor,
					       &castkms_crtc_funcs, NULL);
	if (IS_ERR(castkms_out)) {
		DRM_DEV_ERROR(dev->dev, "Failed to init CRTC\n");
		return castkms_out;
	}

	crtc = &castkms_out->crtc;

	drm_crtc_helper_add(crtc, &castkms_crtc_helper_funcs);

	ret = drm_mode_crtc_set_gamma_size(crtc, CASTKMS_LUT_SIZE);
	if (ret) {
		DRM_ERROR("Failed to set gamma size\n");
		return ERR_PTR(ret);
	}

	drm_crtc_enable_color_mgmt(crtc, 0, false, CASTKMS_LUT_SIZE);

	drm_crtc_attach_background_color_property(crtc);

	spin_lock_init(&castkms_out->lock);
	spin_lock_init(&castkms_out->composer_lock);

	castkms_out->composer_workq = drmm_alloc_ordered_workqueue(dev, "castkms_composer", 0);
	if (IS_ERR(castkms_out->composer_workq))
		return ERR_CAST(castkms_out->composer_workq);

	return castkms_out;
}
