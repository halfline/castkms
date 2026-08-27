/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_FRAME_H_
#define _CASTKMS_FRAME_H_

#include <linux/types.h>

#include <drm/drm_colorop.h>
#include <drm/drm_rect.h>

#include "castkms_colorop.h"

struct drm_framebuffer;
struct iosys_map;

#define CASTKMS_LUT_SIZE 256

/**
 * struct castkms_frame_info - Mapped source image used by one render plane
 * @fb: Backing DRM framebuffer
 * @src: Source rectangle in 16.16 fixed-point framebuffer coordinates
 * @dst: Destination rectangle in whole-pixel output coordinates
 * @map: Borrowed raw per-plane mappings; framebuffer offsets are not applied
 * @rotation: Rotation applied to the source
 *
 * The mappings' owner must outlive this description. @src and @dst have the
 * same dimensions modulo @rotation.
 */
struct castkms_frame_info {
	struct drm_framebuffer *fb;
	struct drm_rect src, dst;
	const struct iosys_map *map;
	unsigned int rotation;
};

struct pixel_argb_s32 {
	s32 a, r, g, b;
};

/**
 * struct pixel_argb_u16 - Internal representation of a pixel color
 * @a: Alpha component in native-endian 16-bit precision
 * @r: Red component in native-endian 16-bit precision
 * @g: Green component in native-endian 16-bit precision
 * @b: Blue component in native-endian 16-bit precision
 *
 * This representation preserves composition precision and keeps color
 * operations simple. It must not be cast directly to an external pixel format.
 */
struct pixel_argb_u16 {
	u16 a, r, g, b;
};

struct line_buffer {
	size_t n_pixels;
	struct pixel_argb_u16 *pixels;
};

typedef void (*pixel_write_t)(u8 *out_pixel,
			      const struct pixel_argb_u16 *in_pixel);

enum pixel_read_direction {
	READ_BOTTOM_TO_TOP,
	READ_TOP_TO_BOTTOM,
	READ_RIGHT_TO_LEFT,
	READ_LEFT_TO_RIGHT,
};

struct castkms_frame_plane;

/**
 * typedef pixel_read_line_t - Read and convert one line from a render plane
 * @plane: Render plane used as the pixel source
 * @x_start: First source x coordinate
 * @y_start: First source y coordinate
 * @direction: Direction in which source pixels are read
 * @count: Number of pixels to read
 * @out_pixel: Destination array with room for @count pixels
 */
typedef void (*pixel_read_line_t)(const struct castkms_frame_plane *plane,
				  int x_start, int y_start,
				  enum pixel_read_direction direction,
				  int count,
				  struct pixel_argb_u16 out_pixel[]);

/**
 * struct conversion_matrix - Matrix for converting a YUV encoding to RGB
 * @matrix: Signed row-major 3x3 matrix with 32 fractional bits
 * @y_offset: Offset applied to the Y value
 */
struct conversion_matrix {
	s64 matrix[3][3];
	int y_offset;
};

/**
 * struct castkms_frame_plane - Renderer-facing state for one plane
 * @frame_info: Source image and placement
 * @pixel_read_line: Format-specific source conversion function
 * @conversion_matrix: YUV-to-RGB conversion matrix
 * @num_colorops: Number of entries in @colorops
 * @colorops: Value-owned color pipeline
 * @zpos: Normalized stacking position
 * @is_cursor: Whether this is the cursor plane
 */
struct castkms_frame_plane {
	struct castkms_frame_info *frame_info;
	pixel_read_line_t pixel_read_line;
	struct conversion_matrix conversion_matrix;
	size_t num_colorops;
	struct castkms_colorop_snapshot *colorops;
	u32 zpos;
	bool is_cursor;
};

struct castkms_color_lut {
	const struct drm_color_lut *base;
	size_t lut_length;
	s64 channel_value2index_ratio;
};

/**
 * struct castkms_frame_stage - Complete renderer input for one frame
 * @planes: Render planes in normalized z-order
 * @num_planes: Number of entries in @planes
 * @gamma_lut: Gamma look-up table applied after plane blending
 * @width: Output width in pixels
 * @height: Output height in pixels
 * @background_color: Background fill color
 * @damage: Bounding box of frame damage in output coordinates
 * @full_damage: Whether @damage covers the complete output
 *
 * A frame stage contains no DRM atomic state. Live commits and owned capture
 * snapshots both produce it, and the renderer consumes only this interface.
 */
struct castkms_frame_stage {
	struct castkms_frame_plane **planes;
	size_t num_planes;
	struct castkms_color_lut gamma_lut;
	u32 width;
	u32 height;
	u64 background_color;
	struct drm_rect damage;
	bool full_damage;
};

#endif /* _CASTKMS_FRAME_H_ */
