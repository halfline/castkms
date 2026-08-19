// SPDX-License-Identifier: GPL-2.0+

#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/slab.h>

#include <drm/drm_fixed.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_framebuffer_helper.h>

#include <kunit/visibility.h>

#include "castkms_capture.h"
#include "castkms_composer.h"
#include "castkms_formats.h"
#include "castkms_output_buffer.h"
#include "castkms_snapshot.h"

struct castkms_snapshot_fence {
	struct dma_fence base;
	spinlock_t lock;
};

static const char *
castkms_snapshot_fence_get_driver_name(struct dma_fence *fence)
{
	(void)fence;
	return "castkms";
}

static const char *
castkms_snapshot_fence_get_timeline_name(struct dma_fence *fence)
{
	(void)fence;
	return "snapshot";
}

static const struct dma_fence_ops castkms_snapshot_fence_ops = {
	.get_driver_name = castkms_snapshot_fence_get_driver_name,
	.get_timeline_name = castkms_snapshot_fence_get_timeline_name,
};

static struct dma_fence *castkms_snapshot_fence_create(void)
{
	struct castkms_snapshot_fence *sf;

	sf = kzalloc(sizeof(*sf), GFP_KERNEL);
	if (!sf)
		return NULL;

	spin_lock_init(&sf->lock);
	dma_fence_init64(&sf->base, &castkms_snapshot_fence_ops,
			 &sf->lock, dma_fence_context_alloc(1), 1);

	return &sf->base;
}

static void castkms_snapshot_fence_signal(struct dma_fence *fence)
{
	bool fence_cookie;

	if (!fence)
		return;

	fence_cookie = dma_fence_begin_signalling();
	dma_fence_signal_timestamp(fence, ktime_get());
	dma_fence_end_signalling(fence_cookie);
	dma_fence_put(fence);
}

static int castkms_snapshot_attach_read_fences(struct castkms_frame_snapshot *snapshot)
{
	struct dma_fence *fence = snapshot->source_fence;
	struct drm_gem_object *seen[DRM_FORMAT_MAX_PLANES * 8];
	int n_seen = 0;
	int i, ret;

	/*
	 * Each resv is locked and unlocked individually — we never hold two
	 * resv locks at once, so ww_acquire_ctx deadlock avoidance is not
	 * needed.  Deduplicate objects so multi-plane formats or shared FBs
	 * don't redundantly lock the same reservation.
	 */
	for (i = 0; i < snapshot->num_planes; i++) {
		struct drm_framebuffer *fb = snapshot->planes[i].frame_info.fb;
		unsigned int j;

		for (j = 0; j < fb->format->num_planes; j++) {
			struct drm_gem_object *obj = fb->obj[j];
			struct dma_fence *dependency = NULL;
			int k;
			bool already_seen = false;

			if (!obj)
				continue;

			for (k = 0; k < n_seen; k++) {
				if (seen[k] == obj) {
					already_seen = true;
					break;
				}
			}
			if (already_seen)
				continue;

			if (WARN_ON(n_seen >= ARRAY_SIZE(seen)))
				return -EOVERFLOW;
			seen[n_seen++] = obj;

			ret = dma_resv_lock(obj->resv, NULL);
			if (ret)
				return ret;

			/*
			 * Snapshot existing writers while holding the reservation lock,
			 * then publish our read fence. Future writers depend on the read
			 * fence; only the captured dependencies may run ahead of us.
			 */
			ret = dma_resv_get_singleton(obj->resv,
						     DMA_RESV_USAGE_WRITE,
						     &dependency);
			if (ret) {
				dma_resv_unlock(obj->resv);
				return ret;
			}

			ret = dma_resv_reserve_fences(obj->resv, 1);
			if (ret) {
				dma_fence_put(dependency);
				dma_resv_unlock(obj->resv);
				return ret;
			}

			dma_resv_add_fence(obj->resv, fence,
					   DMA_RESV_USAGE_READ);
			dma_resv_unlock(obj->resv);

			if (dependency && dma_fence_is_signaled(dependency)) {
				dma_fence_put(dependency);
				dependency = NULL;
			}
			if (dependency)
				snapshot->source_dependencies[
					snapshot->num_source_dependencies++] = dependency;
		}
	}

	return 0;
}

static int
castkms_frame_snapshot_wait_for_sources(struct castkms_frame_snapshot *snapshot)
{
	unsigned int i;

	for (i = 0; i < snapshot->num_source_dependencies; i++) {
		long ret = dma_fence_wait_timeout(snapshot->source_dependencies[i],
						  false,
						  MAX_SCHEDULE_TIMEOUT);

		if (ret <= 0)
			return ret ?: -ETIMEDOUT;
	}

	return 0;
}

static void castkms_frame_snapshot_release(struct kref *kref)
{
	struct castkms_frame_snapshot *snapshot =
		container_of(kref, struct castkms_frame_snapshot, refcount);
	int i;

	/*
	 * Signal the read fence before vunmap: the fence tells writers that
	 * capture is done reading, so they may begin.  The snapshot's own
	 * vmap is independently owned and remains valid until drm_gem_fb_vunmap
	 * drops it below — signaling first is safe because no writer can
	 * invalidate pages that are still vmapped.
	 */
	castkms_snapshot_fence_signal(snapshot->source_fence);

	for (i = 0; i < snapshot->num_planes; i++) {
		struct castkms_snapshot_plane *sp = &snapshot->planes[i];

		kfree(sp->state.colorops);
		drm_gem_fb_vunmap(sp->frame_info.fb, sp->map);
		drm_framebuffer_put(sp->frame_info.fb);
	}
	for (i = 0; i < snapshot->num_source_dependencies; i++)
		dma_fence_put(snapshot->source_dependencies[i]);

	if (snapshot->cursor.fb)
		drm_framebuffer_put(snapshot->cursor.fb);
	kfree(snapshot->gamma_lut_data);
	kfree(snapshot->source_dependencies);
	kfree(snapshot->plane_ptrs);
	kfree(snapshot);
}

struct castkms_frame_snapshot *
castkms_frame_snapshot_create(struct castkms_crtc_state *crtc_state)
{
	struct castkms_frame_snapshot *snapshot;
	int num_planes = crtc_state->num_active_planes;
	int i, ret;

	snapshot = kzalloc(struct_size(snapshot, planes, num_planes), GFP_KERNEL);
	if (!snapshot)
		return ERR_PTR(-ENOMEM);

	kref_init(&snapshot->refcount);
	snapshot->num_planes = 0;

	snapshot->plane_ptrs = kcalloc(num_planes,
				       sizeof(*snapshot->plane_ptrs),
				       GFP_KERNEL);
	if (!snapshot->plane_ptrs) {
		kfree(snapshot);
		return ERR_PTR(-ENOMEM);
	}
	if (num_planes) {
		snapshot->source_dependencies = kcalloc(
			num_planes * DRM_FORMAT_MAX_PLANES,
			sizeof(*snapshot->source_dependencies), GFP_KERNEL);
		if (!snapshot->source_dependencies) {
			kfree(snapshot->plane_ptrs);
			kfree(snapshot);
			return ERR_PTR(-ENOMEM);
		}
	}

	for (i = 0; i < num_planes; i++) {
		struct castkms_plane_state *src = crtc_state->active_planes[i];
		struct castkms_snapshot_plane *sp = &snapshot->planes[i];

		drm_framebuffer_get(src->frame_info->fb);
		ret = drm_gem_fb_vmap(src->frame_info->fb, sp->map, NULL);
		if (ret) {
			drm_framebuffer_put(src->frame_info->fb);
			goto unwind;
		}

		if (!castkms_framebuffer_maps_are_accessible(src->frame_info->fb,
							     sp->map)) {
			drm_gem_fb_vunmap(src->frame_info->fb, sp->map);
			drm_framebuffer_put(src->frame_info->fb);
			ret = -EOPNOTSUPP;
			goto unwind;
		}

		sp->frame_info = *src->frame_info;
		sp->frame_info.map = sp->map;

		sp->state.frame_info = &sp->frame_info;
		sp->state.pixel_read_line = src->pixel_read_line;
		sp->state.conversion_matrix = src->conversion_matrix;
		sp->state.num_colorops = src->num_colorops;
		if (src->colorops && src->num_colorops) {
			sp->state.colorops = kmemdup(src->colorops,
						     src->num_colorops *
						     sizeof(*src->colorops),
						     GFP_KERNEL);
			if (!sp->state.colorops) {
				drm_gem_fb_vunmap(src->frame_info->fb, sp->map);
				drm_framebuffer_put(src->frame_info->fb);
				ret = -ENOMEM;
				goto unwind;
			}
		}

		snapshot->plane_ptrs[i] = &sp->state;
		snapshot->num_planes++;
	}

	snapshot->hdisplay = crtc_state->base.mode.hdisplay;
	snapshot->vdisplay = crtc_state->base.mode.vdisplay;
	snapshot->background_color = crtc_state->base.background_color;
	snapshot->damage_clip = crtc_state->damage_clip;
	snapshot->full_damage = crtc_state->full_damage;
	snapshot->cursor = crtc_state->cursor;
	if (snapshot->cursor.fb)
		drm_framebuffer_get(snapshot->cursor.fb);

	if (crtc_state->gamma_lut.base && crtc_state->gamma_lut.lut_length) {
		size_t lut_size = crtc_state->gamma_lut.lut_length *
				  sizeof(struct drm_color_lut);

		snapshot->gamma_lut_data = kmemdup(crtc_state->gamma_lut.base,
						   lut_size, GFP_KERNEL);
		if (!snapshot->gamma_lut_data) {
			ret = -ENOMEM;
			goto unwind;
		}

		snapshot->gamma_lut.base = snapshot->gamma_lut_data;
		snapshot->gamma_lut.lut_length = crtc_state->gamma_lut.lut_length;
		snapshot->gamma_lut.channel_value2index_ratio =
			crtc_state->gamma_lut.channel_value2index_ratio;
	}

	snapshot->source_fence = castkms_snapshot_fence_create();
	if (!snapshot->source_fence) {
		ret = -ENOMEM;
		goto unwind;
	}

	ret = castkms_snapshot_attach_read_fences(snapshot);
	if (ret)
		goto unwind;

	return snapshot;

unwind:
	castkms_frame_snapshot_release(&snapshot->refcount);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_frame_snapshot_create);

void castkms_frame_snapshot_put(struct castkms_frame_snapshot *snapshot)
{
	kref_put(&snapshot->refcount, castkms_frame_snapshot_release);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_frame_snapshot_put);

struct castkms_capture_job {
	struct work_struct work;
	struct castkms_output *output;
	struct castkms_frame_snapshot *snapshot;
	struct castkms_capture_buffer *buffer;
};

static void capture_composition_worker(struct work_struct *work)
{
	struct castkms_capture_job *job =
		container_of(work, struct castkms_capture_job, work);
	const struct castkms_output_buffer *dest;
	int ret;

	dest = castkms_capture_buffer_output(job->buffer);
	ret = castkms_frame_snapshot_wait_for_sources(job->snapshot);
	if (!ret)
		ret = castkms_compose_snapshot(job->snapshot, dest);
	castkms_capture_buffer_set_damage(job->buffer,
					  &job->snapshot->damage_clip,
					  job->snapshot->full_damage);
	if (!ret)
		ret = castkms_capture_buffer_set_cursor(job->buffer,
						       &job->snapshot->cursor);
	castkms_capture_complete_frame(job->output, job->buffer, ret);
	castkms_frame_snapshot_put(job->snapshot);
	kfree(job);
}

void castkms_capture_queue_job(struct castkms_output *output,
			       struct castkms_capture_buffer *buffer,
			       struct castkms_frame_snapshot *snapshot)
{
	struct castkms_capture_job *job;

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job) {
		castkms_capture_complete_frame(output, buffer, -ENOMEM);
		castkms_frame_snapshot_put(snapshot);
		return;
	}

	INIT_WORK(&job->work, capture_composition_worker);
	job->output = output;
	job->snapshot = snapshot;
	job->buffer = buffer;
	if (!queue_work(output->capture_workq, &job->work)) {
		castkms_capture_complete_frame(output, buffer, -ENODEV);
		castkms_frame_snapshot_put(snapshot);
		kfree(job);
	}
}
