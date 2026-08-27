// SPDX-License-Identifier: GPL-2.0+

#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/slab.h>

#include <drm/drm_fixed.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_plane.h>
#include <drm/drm_gem_framebuffer_helper.h>

#include <kunit/visibility.h>

#include "castkms_formats.h"
#include "castkms_snapshot.h"

struct castkms_snapshot_fence {
	struct dma_fence base;
	spinlock_t lock;
};

/*
 * A broken producer must not make stream teardown or module removal wait
 * forever.  This is one budget for all source fences in a snapshot, rather
 * than a fresh timeout for every plane.
 */
static unsigned int capture_source_fence_timeout_ms = 5000;
module_param_named(capture_source_fence_timeout_ms,
		   capture_source_fence_timeout_ms, uint, 0444);
MODULE_PARM_DESC(capture_source_fence_timeout_ms,
		 "Maximum total wait for a capture snapshot's source fences (ms)");

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

static int castkms_snapshot_attach_framebuffer_read_fences(
	struct castkms_frame_snapshot *snapshot, struct drm_framebuffer *fb,
	struct drm_gem_object **seen, unsigned int *n_seen,
	unsigned int seen_capacity)
{
	struct dma_fence *fence = snapshot->source_fence;
	unsigned int j;
	int ret;

	/*
	 * Each resv is locked and unlocked individually. We never hold two
	 * resv locks at once, so ww_acquire_ctx deadlock avoidance is not
	 * needed.  Deduplicate objects so multi-plane formats or shared FBs
	 * don't redundantly lock the same reservation.
	 */
	for (j = 0; j < fb->format->num_planes; j++) {
		struct drm_gem_object *obj = fb->obj[j];
		struct dma_fence *dependency = NULL;
		unsigned int k;
		bool already_seen = false;

		if (!obj)
			continue;

		for (k = 0; k < *n_seen; k++) {
			if (seen[k] == obj) {
				already_seen = true;
				break;
			}
		}
		if (already_seen)
			continue;

		if (WARN_ON(*n_seen >= seen_capacity))
			return -EOVERFLOW;
		seen[(*n_seen)++] = obj;

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

		dma_resv_add_fence(obj->resv, fence, DMA_RESV_USAGE_READ);
		dma_resv_unlock(obj->resv);

		if (dependency && dma_fence_is_signaled(dependency)) {
			dma_fence_put(dependency);
			dependency = NULL;
		}
		if (dependency)
			snapshot->source_dependencies[
				snapshot->num_source_dependencies++] = dependency;
	}

	return 0;
}

static int castkms_snapshot_attach_read_fences(
	struct castkms_frame_snapshot *snapshot)
{
	unsigned int capacity =
		(snapshot->frame.num_planes + !!snapshot->frame.cursor.fb) *
		DRM_FORMAT_MAX_PLANES;
	struct drm_gem_object **seen;
	unsigned int n_seen = 0;
	int i;
	int ret = 0;

	if (!capacity)
		return 0;

	seen = kcalloc(capacity, sizeof(*seen), GFP_KERNEL);
	if (!seen)
		return -ENOMEM;

	for (i = 0; i < snapshot->frame.num_planes; i++) {
		ret = castkms_snapshot_attach_framebuffer_read_fences(
			snapshot, snapshot->planes[i].frame_info.fb, seen,
			&n_seen, capacity);
		if (ret)
			goto out;
	}

	/* Cursor metadata is copied after composition and needs the same barrier. */
	if (snapshot->frame.cursor.fb)
		ret = castkms_snapshot_attach_framebuffer_read_fences(
			snapshot, snapshot->frame.cursor.fb, seen, &n_seen,
			capacity);

out:
	kfree(seen);
	return ret;
}

VISIBLE_IF_KUNIT int castkms_snapshot_wait_fences(
	struct dma_fence **fences, unsigned int count, long timeout)
{
	long remaining = max_t(long, 1, timeout);
	unsigned int i;

	for (i = 0; i < count; i++) {
		remaining = dma_fence_wait_timeout(fences[i], false, remaining);

		if (remaining <= 0)
			return remaining ?: -ETIMEDOUT;
	}

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_snapshot_wait_fences);

int
castkms_frame_snapshot_wait_for_sources(struct castkms_frame_snapshot *snapshot)
{
	return castkms_snapshot_wait_fences(
		snapshot->source_dependencies,
		snapshot->num_source_dependencies,
		msecs_to_jiffies(capture_source_fence_timeout_ms));
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
	 * drops it below. Signaling first is safe because no writer can
	 * invalidate pages that are still vmapped.
	 */
	castkms_snapshot_fence_signal(snapshot->source_fence);

	for (i = 0; i < snapshot->frame.num_planes; i++) {
		struct castkms_snapshot_plane *sp = &snapshot->planes[i];

		kfree(sp->plane.colorops);
		drm_gem_fb_vunmap(sp->frame_info.fb, sp->map);
		drm_framebuffer_put(sp->frame_info.fb);
	}
	for (i = 0; i < snapshot->num_source_dependencies; i++)
		dma_fence_put(snapshot->source_dependencies[i]);

	if (snapshot->frame.cursor.fb)
		drm_framebuffer_put(snapshot->frame.cursor.fb);
	kfree(snapshot->gamma_lut_data);
	kfree(snapshot->source_dependencies);
	kfree(snapshot->frame.planes);
	kfree(snapshot);
}

struct castkms_frame_snapshot *
castkms_frame_snapshot_create(const struct castkms_frame_stage *frame)
{
	struct castkms_frame_snapshot *snapshot;
	int num_planes = frame->num_planes;
	int i, ret;

	snapshot = kzalloc(struct_size(snapshot, planes, num_planes), GFP_KERNEL);
	if (!snapshot)
		return ERR_PTR(-ENOMEM);

	kref_init(&snapshot->refcount);
	snapshot->frame = *frame;
	snapshot->frame.num_planes = 0;
	snapshot->frame.gamma_lut = (struct castkms_color_lut) {};

	snapshot->frame.planes = kcalloc(num_planes,
					 sizeof(*snapshot->frame.planes),
					 GFP_KERNEL);
	if (!snapshot->frame.planes) {
		kfree(snapshot);
		return ERR_PTR(-ENOMEM);
	}
	if (num_planes || frame->cursor.fb) {
		snapshot->source_dependencies = kcalloc(
			(num_planes + !!frame->cursor.fb) *
				DRM_FORMAT_MAX_PLANES,
			sizeof(*snapshot->source_dependencies), GFP_KERNEL);
		if (!snapshot->source_dependencies) {
			kfree(snapshot->frame.planes);
			kfree(snapshot);
			return ERR_PTR(-ENOMEM);
		}
	}
	if (snapshot->frame.cursor.fb)
		drm_framebuffer_get(snapshot->frame.cursor.fb);

	for (i = 0; i < num_planes; i++) {
		struct castkms_frame_plane *src = frame->planes[i];
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

		sp->plane = *src;
		sp->plane.frame_info = &sp->frame_info;
		sp->plane.colorops = NULL;
		if (src->colorops && src->num_colorops) {
			sp->plane.colorops = kmemdup(src->colorops,
						     src->num_colorops *
						     sizeof(*src->colorops),
						     GFP_KERNEL);
			if (!sp->plane.colorops) {
				drm_gem_fb_vunmap(src->frame_info->fb, sp->map);
				drm_framebuffer_put(src->frame_info->fb);
				ret = -ENOMEM;
				goto unwind;
			}
		}

		snapshot->frame.planes[i] = &sp->plane;
		snapshot->frame.num_planes++;
	}

	if (frame->gamma_lut.base && frame->gamma_lut.lut_length) {
		size_t lut_size = frame->gamma_lut.lut_length *
				  sizeof(struct drm_color_lut);

		snapshot->gamma_lut_data = kmemdup(frame->gamma_lut.base,
						   lut_size, GFP_KERNEL);
		if (!snapshot->gamma_lut_data) {
			ret = -ENOMEM;
			goto unwind;
		}

		snapshot->frame.gamma_lut.base = snapshot->gamma_lut_data;
		snapshot->frame.gamma_lut.lut_length =
			frame->gamma_lut.lut_length;
		snapshot->frame.gamma_lut.channel_value2index_ratio =
			frame->gamma_lut.channel_value2index_ratio;
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
