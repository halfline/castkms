/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_OUTPUT_BUFFER_H_
#define _CASTKMS_OUTPUT_BUFFER_H_

#include "castkms_drv.h"

/**
 * struct castkms_output_buffer - Mapped destination for composed pixels
 * @fb: Owned reference to the destination framebuffer
 * @map: Owned per-plane mappings for @fb
 * @write_pixel: Format-specific conversion for one composed pixel
 *
 * The framebuffer reference and mappings are owned from successful
 * castkms_output_buffer_init() until castkms_output_buffer_fini(). CPU writes
 * must be bracketed by the begin and end access functions.
 */
struct castkms_output_buffer {
	struct drm_framebuffer *fb;
	struct iosys_map map[DRM_FORMAT_MAX_PLANES];
	pixel_write_t write_pixel;
};

int castkms_output_buffer_init(struct castkms_output_buffer *buffer,
			       struct drm_framebuffer *fb);
void castkms_output_buffer_fini(struct castkms_output_buffer *buffer);
bool castkms_output_buffer_is_valid(const struct castkms_output_buffer *buffer);
int castkms_output_buffer_begin_cpu_access(const struct castkms_output_buffer *buffer);
void castkms_output_buffer_end_cpu_access(const struct castkms_output_buffer *buffer);
void castkms_output_buffer_write_row(const struct castkms_output_buffer *buffer,
				     const struct line_buffer *src_buffer,
				     int y);

#endif /* _CASTKMS_OUTPUT_BUFFER_H_ */
