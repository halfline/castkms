/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_SNAPSHOT_H_
#define _CASTKMS_SNAPSHOT_H_

#include <linux/kref.h>

#include <linux/dma-fence.h>

#include <drm/drm_gem_atomic_helper.h>

#include "castkms_drv.h"

/**
 * struct castkms_snapshot_plane - Owned copy of one plane's composition state
 * @state: Minimal plane state with pixel_read_line, conversion_matrix, and
 *         an independently owned colorops array. The embedded
 *         drm_shadow_plane_state base is zeroed and unused.
 * @frame_info: Owned frame description; fb carries a drm_framebuffer_get()
 *              reference, and map points to @map below.
 * @map: Independently vmapped buffer-object mappings for @frame_info.fb
 */
struct castkms_snapshot_plane {
	struct castkms_plane_state state;
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
 * @num_planes: Number of active planes in this snapshot
 * @plane_ptrs: Array of pointers into @planes[].state, satisfying the
 *              castkms_plane_state** interface used by compose_active_planes()
 * @gamma_lut: Gamma look-up table for this frame
 * @gamma_lut_data: Independently owned copy of the LUT entries
 * @hdisplay: Horizontal display size from the CRTC mode
 * @vdisplay: Vertical display size from the CRTC mode
 * @background_color: Background fill color from the CRTC state
 * @damage_clip: Bounding box of frame damage in CRTC coordinates
 * @full_damage: True when damage covers the entire frame
 * @planes: Flexible array of per-plane snapshots with owned resources
 */
struct castkms_frame_snapshot {
	struct kref refcount;
	struct dma_fence *source_fence;
	struct dma_fence **source_dependencies;
	unsigned int num_source_dependencies;
	int num_planes;
	struct castkms_plane_state **plane_ptrs;
	struct castkms_color_lut gamma_lut;
	struct drm_color_lut *gamma_lut_data;
	u32 hdisplay, vdisplay;
	u64 background_color;
	struct drm_rect damage_clip;
	bool full_damage;
	struct castkms_cursor_snapshot cursor;
	struct castkms_snapshot_plane planes[];
};

struct castkms_capture_buffer;

struct castkms_frame_snapshot *
castkms_frame_snapshot_create(struct castkms_crtc_state *crtc_state);

void castkms_frame_snapshot_put(struct castkms_frame_snapshot *snapshot);

void castkms_capture_queue_job(struct castkms_output *output,
			       struct castkms_capture_buffer *buffer,
			       struct castkms_frame_snapshot *snapshot);

#endif /* _CASTKMS_SNAPSHOT_H_ */
