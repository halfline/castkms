/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_SNAPSHOT_H_
#define _CASTKMS_SNAPSHOT_H_

#include <linux/dma-fence.h>
#include <linux/iosys-map.h>
#include <linux/kref.h>

#include <drm/drm_fourcc.h>

#include "castkms_frame.h"

/**
 * struct castkms_snapshot_plane - Owned copy of one plane's composition state
 * @plane: Independently owned renderer plane and colorops array
 * @frame_info: Owned frame description; fb carries a drm_framebuffer_get()
 *              reference, and map points to @map below.
 * @map: Independently vmapped buffer-object mappings for @frame_info.fb
 */
struct castkms_snapshot_plane {
	struct castkms_frame_plane plane;
	struct castkms_frame_info frame_info;
	struct iosys_map map[DRM_FORMAT_MAX_PLANES];
};

/**
 * struct castkms_frame_snapshot - Immutable, refcounted snapshot of a composed frame
 * @refcount: Reference count for shared ownership
 * @source_fence: Read fence published on each source BO's dma_resv, signaled
 *                when the snapshot is released so writers know capture is done
 * @source_dependencies: Writer fences that existed before @source_fence was
 *                       published and must resolve before composition
 * @num_source_dependencies: Number of entries in @source_dependencies
 * @frame: Immutable renderer input whose planes point into @planes
 * @gamma_lut_data: Independently owned copy of the LUT entries
 * @planes: Flexible array of per-plane snapshots with owned resources
 */
struct castkms_frame_snapshot {
	struct kref refcount;
	struct dma_fence *source_fence;
	struct dma_fence **source_dependencies;
	unsigned int num_source_dependencies;
	struct castkms_frame_stage frame;
	struct drm_color_lut *gamma_lut_data;
	struct castkms_snapshot_plane planes[];
};

struct castkms_frame_snapshot *
castkms_frame_snapshot_create(const struct castkms_frame_stage *frame);
int castkms_frame_snapshot_wait_for_sources(
	struct castkms_frame_snapshot *snapshot);

#if IS_ENABLED(CONFIG_KUNIT)
int castkms_snapshot_wait_fences(
	struct dma_fence **fences, unsigned int count, long timeout);
#endif

void castkms_frame_snapshot_put(struct castkms_frame_snapshot *snapshot);

#endif /* _CASTKMS_SNAPSHOT_H_ */
