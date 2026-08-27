/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_COLOROP_H_
#define _CASTKMS_COLOROP_H_

#include <linux/types.h>

#include <drm/drm_colorop.h>

/**
 * struct castkms_colorop_snapshot - value-owned color-operation state
 * @type: Operation selected by the plane's color pipeline
 * @bypass: Whether this operation is disabled
 * @curve_1d_type: Transfer curve selected for a 1D curve operation
 * @has_ctm: Whether @ctm contains matrix data
 * @ctm: Value-owned matrix for a 3x4 CTM operation
 */
struct castkms_colorop_snapshot {
	enum drm_colorop_type type;
	bool bypass;
	enum drm_colorop_curve_1d_type curve_1d_type;
	bool has_ctm;
	struct drm_color_ctm_3x4 ctm;
};

struct drm_colorop;
struct drm_colorop_state;
struct drm_plane;

int castkms_initialize_colorops(struct drm_plane *plane);
int castkms_colorop_snapshot_init(struct castkms_colorop_snapshot *snapshot,
				  const struct drm_colorop *colorop,
				  const struct drm_colorop_state *state);

#endif /* _CASTKMS_COLOROP_H_ */
