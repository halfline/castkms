/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_FORMATS_H_
#define _CASTKMS_FORMATS_H_

#include "castkms_drv.h"

int castkms_plane_formats_alloc(u32 **formats);

pixel_read_line_t castkms_get_pixel_read_line_function(u32 format);

pixel_write_t castkms_get_pixel_write_function(u32 format);
bool castkms_framebuffer_read_strides_are_valid(const struct drm_framebuffer *fb);

void castkms_get_conversion_matrix_to_argb_u16(u32 format, enum drm_color_encoding encoding,
				       enum drm_color_range range,
				       struct conversion_matrix *matrix);

#if IS_ENABLED(CONFIG_KUNIT)
size_t castkms_packed_pixels_offset(const struct castkms_frame_info *frame_info,
				    int x, int y, unsigned int plane_index,
				    int *rem_x, int *rem_y);
struct pixel_argb_u16 castkms_argb_u16_from_yuv161616(const struct conversion_matrix *matrix,
					      u16 y, u16 channel_1, u16 channel_2);
bool castkms_format_registries_are_valid(void);
#endif

#endif /* _CASTKMS_FORMATS_H_ */
