/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_COMPOSER_H_
#define _CASTKMS_COMPOSER_H_

#include <kunit/visibility.h>
#include "castkms_drv.h"

/*
 * This enum is related to the positions of the variables inside
 * `struct drm_color_lut`, so the order of both needs to be the same.
 */
enum lut_channel {
	LUT_RED = 0,
	LUT_GREEN,
	LUT_BLUE,
	LUT_RESERVED
};

#if IS_ENABLED(CONFIG_KUNIT)
void castkms_apply_colorops(const struct castkms_plane_state *plane_state,
			    struct line_buffer *output_buffer);
int castkms_composer_demand_get(struct castkms_composer_demand *demand,
				enum castkms_composer_client client,
				int vblank_ret, bool *keep_vblank);
bool castkms_composer_demand_put(struct castkms_composer_demand *demand,
				 enum castkms_composer_client client);
u16 castkms_lerp_u16(u16 a, u16 b, s64 t);
s64 castkms_get_lut_index(const struct castkms_color_lut *lut, u16 channel_value);
u16 castkms_apply_lut_to_channel_value(const struct castkms_color_lut *lut,
				       s32 channel_value,
				       enum lut_channel channel);
void castkms_apply_3x4_matrix(struct pixel_argb_s32 *pixel, const struct drm_color_ctm_3x4 *matrix);
#endif

#endif /* _CASTKMS_COMPOSER_H_ */
