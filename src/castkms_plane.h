/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_PLANE_H_
#define _CASTKMS_PLANE_H_

#include <linux/container_of.h>

#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_plane.h>

#include "castkms_frame.h"

struct castkms_config_plane;
struct castkms_device;
struct drm_atomic_commit;

/**
 * struct castkms_plane_state - Driver-specific atomic plane state
 * @base: DRM shadow-plane state
 * @frame: Renderer-facing state produced by this atomic state
 */
struct castkms_plane_state {
	struct drm_shadow_plane_state base;
	union {
		struct castkms_frame_plane frame;
		struct {
			struct castkms_frame_info *frame_info;
			pixel_read_line_t pixel_read_line;
			struct conversion_matrix conversion_matrix;
			u32 zpos;
			bool is_cursor;
		};
	};
};

/**
 * struct castkms_plane - Driver-specific plane
 * @base: Base DRM plane
 */
struct castkms_plane {
	struct drm_plane base;
};

#define to_castkms_plane_state(target) \
	container_of(target, struct castkms_plane_state, base.base)

struct castkms_plane *
castkms_plane_init(struct castkms_device *castkmsdev,
		   struct castkms_config_plane *plane_cfg);
int castkms_plane_snapshot_colorops(struct castkms_plane_state *plane_state,
				    struct drm_atomic_commit *state);

#endif /* _CASTKMS_PLANE_H_ */
