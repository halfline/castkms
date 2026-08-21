// SPDX-License-Identifier: GPL-2.0+

#include <linux/atomic.h>
#include <linux/dma-fence.h>
#include <linux/sort.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_auth.h>
#include <drm/drm_blend.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>
#include <drm/drm_vblank_helper.h>

#include <kunit/visibility.h>

#include "castkms_capture_authority.h"
#include "castkms_drv.h"
#include "castkms_framebuffer.h"

static int castkms_plane_state_zpos_cmp(const void *a, const void *b)
{
	const struct castkms_plane_state *plane_a =
		*(const struct castkms_plane_state **)a;
	const struct castkms_plane_state *plane_b =
		*(const struct castkms_plane_state **)b;
	u32 zpos_a = plane_a->base.base.normalized_zpos;
	u32 zpos_b = plane_b->base.base.normalized_zpos;

	return (zpos_a > zpos_b) - (zpos_a < zpos_b);
}

VISIBLE_IF_KUNIT void
castkms_sort_plane_states(struct castkms_plane_state **planes, size_t count)
{
	sort(planes, count, sizeof(*planes), castkms_plane_state_zpos_cmp,
	     NULL);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_sort_plane_states);

VISIBLE_IF_KUNIT bool
castkms_capture_blank_establishes_owner(bool old_state_exists,
					bool old_had_visible_planes,
					bool mode_changed,
					bool active_changed,
					bool background_changed)
{
	return !old_state_exists || old_had_visible_planes || mode_changed ||
	       active_changed || background_changed;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_capture_blank_establishes_owner);

static bool castkms_crtc_handle_vblank_timeout(struct drm_crtc *crtc)
{
	struct castkms_output *output = drm_crtc_to_castkms_output(crtc);
	struct castkms_crtc_state *state;
	bool queue_composer = false;
	bool ret, fence_cookie;

	fence_cookie = dma_fence_begin_signalling();

	spin_lock(&output->lock);
	ret = drm_crtc_handle_vblank(crtc);
	if (!ret)
		DRM_ERROR("castkms failure on handling vblank");

	state = output->composer_state;
	if (state && castkms_composer_demand_is_active(&output->composer_demand)) {
		ktime_t frame_time;
		u64 frame = drm_crtc_vblank_count_and_time(crtc, &frame_time);

		if (output->composer_demand.crc_enabled &&
		    castkms_capture_output_content_is_safe_locked(output)) {
			/* Update frame_start only after the worker consumed it. */
			spin_lock(&output->composer_lock);
			if (!state->crc_pending)
				state->frame_start = frame;
			else
				DRM_DEBUG_DRIVER("crc worker behind: start=%llu end=%llu\n",
						 state->frame_start, frame);
			state->frame_end = frame;
			state->crc_pending = true;
			spin_unlock(&output->composer_lock);
			queue_composer = true;
		}

		if (castkms_capture_prepare_frame(output, state, frame,
						  frame_time))
			queue_composer = true;

		if (queue_composer &&
		    !queue_work(output->composer_workq, &state->composer_work))
			DRM_DEBUG_DRIVER("Composer worker already queued\n");
	}
	spin_unlock(&output->lock);

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
	castkms_state->capture_owner =
		to_castkms_crtc_state(crtc->state)->capture_owner;
	if (castkms_state->capture_owner)
		drm_master_get(castkms_state->capture_owner);

	INIT_WORK(&castkms_state->composer_work, castkms_composer_worker);

	return &castkms_state->base;
}

static void castkms_atomic_crtc_destroy_state(struct drm_crtc *crtc,
					   struct drm_crtc_state *state)
{
	struct castkms_crtc_state *castkms_state = to_castkms_crtc_state(state);

	if (castkms_state->capture_owner)
		drm_master_put(&castkms_state->capture_owner);
	__drm_atomic_helper_crtc_destroy_state(state);

	WARN_ON(work_pending(&castkms_state->composer_work));
	if (castkms_state->cursor.fb)
		drm_framebuffer_put(castkms_state->cursor.fb);
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

static atomic_t cursor_serial_counter = ATOMIC_INIT(0);

static void castkms_snapshot_cursor(struct castkms_crtc_state *castkms_state,
				    struct drm_atomic_state *state,
				    struct drm_crtc *crtc)
{
	struct drm_crtc_state *crtc_state = &castkms_state->base;
	struct castkms_cursor_snapshot *cursor = &castkms_state->cursor;
	struct drm_plane *plane;

	memset(cursor, 0, sizeof(*cursor));

	drm_for_each_plane_mask(plane, crtc->dev, crtc_state->plane_mask) {
		struct drm_plane_state *new_ps, *old_ps;
		bool image_changed;

		if (plane->type != DRM_PLANE_TYPE_CURSOR)
			continue;

		new_ps = drm_atomic_get_new_plane_state(state, plane);
		if (!new_ps || !new_ps->visible)
			break;

		cursor->visible = true;
		cursor->x = new_ps->crtc_x;
		cursor->y = new_ps->crtc_y;
		cursor->hotspot_x = new_ps->hotspot_x;
		cursor->hotspot_y = new_ps->hotspot_y;
		cursor->width = new_ps->fb ? new_ps->fb->width : 0;
		cursor->height = new_ps->fb ? new_ps->fb->height : 0;

		if (new_ps->fb) {
			cursor->fb = new_ps->fb;
			drm_framebuffer_get(cursor->fb);
		}

		old_ps = drm_atomic_get_old_plane_state(state, plane);
		image_changed = !old_ps || !old_ps->visible ||
				old_ps->fb != new_ps->fb ||
				!drm_rect_equals(&old_ps->src, &new_ps->src) ||
				old_ps->hotspot_x != new_ps->hotspot_x ||
				old_ps->hotspot_y != new_ps->hotspot_y;
		cursor->serial = image_changed ?
			(u32)atomic_inc_return(&cursor_serial_counter) :
			(old_ps ? to_castkms_crtc_state(
				drm_atomic_get_old_crtc_state(state, crtc))->cursor.serial : 0);
		break;
	}
}

static void castkms_compute_frame_damage(struct castkms_crtc_state *castkms_state,
					 struct drm_atomic_state *state,
					 struct drm_crtc *crtc)
{
	struct drm_crtc_state *old_crtc_state =
		drm_atomic_get_old_crtc_state(state, crtc);
	struct drm_crtc_state *new_crtc_state = &castkms_state->base;
	int hdisplay = new_crtc_state->mode.hdisplay;
	int vdisplay = new_crtc_state->mode.vdisplay;
	struct drm_rect merged = {};
	bool has_clips = false;
	int i;

	castkms_state->damage_clip = (struct drm_rect){
		.x1 = 0, .y1 = 0,
		.x2 = hdisplay, .y2 = vdisplay,
	};
	castkms_state->full_damage = true;

	if (!old_crtc_state ||
	    old_crtc_state->plane_mask != new_crtc_state->plane_mask ||
	    new_crtc_state->mode_changed || new_crtc_state->active_changed ||
	    old_crtc_state->background_color != new_crtc_state->background_color ||
	    old_crtc_state->gamma_lut != new_crtc_state->gamma_lut ||
	    old_crtc_state->degamma_lut != new_crtc_state->degamma_lut)
		return;

	for (i = 0; i < castkms_state->num_active_planes; i++) {
		struct castkms_plane_state *ps = castkms_state->active_planes[i];
		struct drm_plane_state *new_ps = &ps->base.base;
		struct drm_plane_state *old_ps =
			drm_atomic_get_old_plane_state(state, new_ps->plane);
		struct drm_rect clip;
		int off_x, off_y;

		if (!old_ps)
			return;

		if (new_ps->rotation != DRM_MODE_ROTATE_0)
			return;

		if (!drm_rect_equals(&old_ps->dst, &new_ps->dst) ||
		    old_ps->alpha != new_ps->alpha ||
		    old_ps->pixel_blend_mode != new_ps->pixel_blend_mode ||
		    old_ps->color_encoding != new_ps->color_encoding ||
		    old_ps->color_range != new_ps->color_range)
			return;

		if (!drm_atomic_helper_damage_merged(old_ps, new_ps, &clip))
			continue;

		off_x = new_ps->dst.x1 - (new_ps->src.x1 >> 16);
		off_y = new_ps->dst.y1 - (new_ps->src.y1 >> 16);
		clip.x1 += off_x;
		clip.y1 += off_y;
		clip.x2 += off_x;
		clip.y2 += off_y;

		if (!has_clips) {
			merged = clip;
			has_clips = true;
		} else {
			merged.x1 = min(merged.x1, clip.x1);
			merged.y1 = min(merged.y1, clip.y1);
			merged.x2 = max(merged.x2, clip.x2);
			merged.y2 = max(merged.y2, clip.y2);
		}
	}

	if (!has_clips)
		return;

	merged.x1 = clamp(merged.x1, 0, hdisplay);
	merged.y1 = clamp(merged.y1, 0, vdisplay);
	merged.x2 = clamp(merged.x2, 0, hdisplay);
	merged.y2 = clamp(merged.y2, 0, vdisplay);

	if (merged.x1 == 0 && merged.y1 == 0 &&
	    merged.x2 == hdisplay && merged.y2 == vdisplay)
		return;

	castkms_state->damage_clip = merged;
	castkms_state->full_damage = false;
}

static int castkms_crtc_atomic_check(struct drm_crtc *crtc,
				  struct drm_atomic_state *state)
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
		struct castkms_plane_state *castkms_plane_state;

		plane_state = drm_atomic_get_new_plane_state(crtc_state->state, plane);
		if (WARN_ON(!plane_state))
			return -EINVAL;

		if (!plane_state->visible)
			continue;

		castkms_plane_state = to_castkms_plane_state(plane_state);
		ret = castkms_plane_snapshot_colorops(castkms_plane_state, state);
		if (ret)
			return ret;

		i++;
	}

	castkms_state->active_planes = kzalloc_objs(*castkms_state->active_planes, i);
	if (!castkms_state->active_planes)
		return -ENOMEM;
	castkms_state->num_active_planes = i;

	i = 0;
	drm_for_each_plane_mask(plane, crtc->dev, crtc_state->plane_mask) {
		plane_state = drm_atomic_get_new_plane_state(crtc_state->state, plane);
		if (WARN_ON(!plane_state)) {
			ret = -EINVAL;
			goto err_free_active_planes;
		}

		if (!plane_state->visible)
			continue;

		castkms_state->active_planes[i++] =
			to_castkms_plane_state(plane_state);
	}

	castkms_sort_plane_states(castkms_state->active_planes,
				  castkms_state->num_active_planes);

	if (castkms_state->num_active_planes) {
		struct drm_master *owner = NULL;

		for (i = 0; i < castkms_state->num_active_planes; i++) {
			struct drm_framebuffer *fb =
				castkms_state->active_planes[i]->base.base.fb;
			struct drm_master *plane_owner =
				castkms_framebuffer_capture_owner(fb);

			if (!plane_owner || (owner && owner != plane_owner)) {
				owner = NULL;
				break;
			}
			owner = plane_owner;
		}

		if (owner != castkms_state->capture_owner) {
			if (owner)
				drm_master_get(owner);
			if (castkms_state->capture_owner)
				drm_master_put(&castkms_state->capture_owner);
			castkms_state->capture_owner = owner;
		}
	} else if (!crtc_state->active) {
		if (castkms_state->capture_owner)
			drm_master_put(&castkms_state->capture_owner);
	} else {
		struct drm_crtc_state *old_crtc_state =
			drm_atomic_get_old_crtc_state(state, crtc);
		struct castkms_crtc_state *old_castkms_state = old_crtc_state ?
			to_castkms_crtc_state(old_crtc_state) : NULL;
		bool background_changed = old_crtc_state &&
			old_crtc_state->background_color !=
				crtc_state->background_color;
		bool had_visible_planes = old_castkms_state &&
			old_castkms_state->num_active_planes;
		bool establishes_blank;

		establishes_blank =
			castkms_capture_blank_establishes_owner(!!old_crtc_state,
								had_visible_planes,
								crtc_state->mode_changed,
								crtc_state->active_changed,
								background_changed);

		if (establishes_blank) {
			struct drm_master *owner =
				castkms_capture_authority_current_master_get(crtc->dev);

			if (castkms_state->capture_owner)
				drm_master_put(&castkms_state->capture_owner);
			castkms_state->capture_owner = owner;
		}
	}

	castkms_compute_frame_damage(castkms_state, state, crtc);
	castkms_snapshot_cursor(castkms_state, state, crtc);

	return 0;

err_free_active_planes:
	kfree(castkms_state->active_planes);
	castkms_state->active_planes = NULL;
	castkms_state->num_active_planes = 0;
	return ret;
}

static void castkms_crtc_atomic_begin(struct drm_crtc *crtc,
				   struct drm_atomic_state *state)
	__acquires(&castkms_output->lock)
{
	struct castkms_output *castkms_output = drm_crtc_to_castkms_output(crtc);

	/* This lock is held across the atomic commit to block vblank timer
	 * from scheduling castkms_composer_worker until the composer is updated
	 */
	spin_lock_irq(&castkms_output->lock);
	castkms_output->capture_owner_updating = true;
}

static void castkms_crtc_atomic_flush(struct drm_crtc *crtc,
				   struct drm_atomic_state *state)
	__releases(&castkms_output->lock)
{
	struct castkms_output *castkms_output = drm_crtc_to_castkms_output(crtc);
	struct castkms_capture_completion capture_completion = {};
	bool capture_cancelled = false;

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
	if (crtc->state->mode_changed || crtc->state->active_changed ||
	    crtc->state->connectors_changed)
		capture_cancelled =
			castkms_capture_mode_changed(castkms_output, crtc->state,
						     &capture_completion);

	spin_unlock_irq(&castkms_output->lock);

	if (capture_cancelled)
		castkms_composer_put(castkms_output,
				     CASTKMS_COMPOSER_CLIENT_CAPTURE);
	castkms_capture_send_completion(castkms_output, &capture_completion);
}

static const struct drm_crtc_helper_funcs castkms_crtc_helper_funcs = {
	.atomic_check	= castkms_crtc_atomic_check,
	.atomic_begin	= castkms_crtc_atomic_begin,
	.atomic_flush	= castkms_crtc_atomic_flush,
	.atomic_enable	= drm_crtc_vblank_atomic_enable,
	.atomic_disable	= drm_crtc_vblank_atomic_disable,
	.handle_vblank_timeout = castkms_crtc_handle_vblank_timeout,
};

static void castkms_crtc_capture_owner_cleanup(struct drm_device *dev,
					       void *data)
{
	struct castkms_output *output = data;
	struct drm_master *owner;
	unsigned long flags;

	(void)dev;

	spin_lock_irqsave(&output->lock, flags);
	owner = output->capture_owner;
	output->capture_owner = NULL;
	output->capture_owner_updating = true;
	spin_unlock_irqrestore(&output->lock, flags);
	if (owner)
		drm_master_put(&owner);
}

struct castkms_output *castkms_crtc_init(struct drm_device *dev, struct drm_plane *primary,
				   struct drm_plane *cursor)
{
	struct castkms_output *castkms_out;
	struct drm_crtc *crtc;
	int err;

	castkms_out = drmm_crtc_alloc_with_planes(dev, struct castkms_output, crtc,
					       primary, cursor,
					       &castkms_crtc_funcs, NULL);
	if (IS_ERR(castkms_out)) {
		DRM_DEV_ERROR(dev->dev, "Failed to init CRTC\n");
		return castkms_out;
	}

	crtc = &castkms_out->crtc;

	drm_crtc_helper_add(crtc, &castkms_crtc_helper_funcs);

	err = drm_mode_crtc_set_gamma_size(crtc, CASTKMS_LUT_SIZE);
	if (err) {
		DRM_ERROR("Failed to set gamma size\n");
		return ERR_PTR(err);
	}

	drm_crtc_enable_color_mgmt(crtc, 0, false, CASTKMS_LUT_SIZE);

	drm_crtc_attach_background_color_property(crtc);

	spin_lock_init(&castkms_out->lock);
	spin_lock_init(&castkms_out->composer_lock);
	err = drmm_add_action_or_reset(dev,
				       castkms_crtc_capture_owner_cleanup,
				       castkms_out);
	if (err)
		return ERR_PTR(err);
	err = castkms_capture_output_init(dev, castkms_out);
	if (err)
		return ERR_PTR(err);

	/*
	 * capture_workq MUST be allocated before composer_workq.  drmm
	 * teardown is LIFO, so the later allocation is destroyed first.
	 * Destroying composer_workq first drains any in-flight composer
	 * work that might queue a capture job; the capture_workq is still
	 * alive at that point and drains second.  Reversing the order
	 * would destroy capture_workq while the composer could still
	 * enqueue onto it.
	 */
	castkms_out->capture_workq = drmm_alloc_ordered_workqueue(dev, "castkms_capture", 0);
	if (IS_ERR(castkms_out->capture_workq))
		return ERR_CAST(castkms_out->capture_workq);

	castkms_out->composer_workq = drmm_alloc_ordered_workqueue(dev, "castkms_composer", 0);
	if (IS_ERR(castkms_out->composer_workq))
		return ERR_CAST(castkms_out->composer_workq);

	return castkms_out;
}
