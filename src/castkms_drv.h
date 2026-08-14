/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_DRV_H_
#define _CASTKMS_DRV_H_

#include <linux/hrtimer.h>

#include <drm/drm.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_encoder.h>
#include <drm/drm_writeback.h>

#define DEFAULT_DEVICE_NAME "castkms"

#define XRES_MIN    10
#define YRES_MIN    10

#define XRES_DEF  1024
#define YRES_DEF   768

#define XRES_MAX  8192
#define YRES_MAX  8192

#define NUM_OVERLAY_PLANES 8
#define CASTKMS_MAX_OUTPUT_OBJECTS 31

#define CASTKMS_LUT_SIZE 256

/**
 * struct castkms_frame_info - Structure to store the state of a frame
 *
 * @fb: backing drm framebuffer
 * @src: source rectangle of this frame in the source framebuffer, stored in 16.16 fixed-point form
 * @dst: destination rectangle in the crtc buffer, stored in whole pixel units
 * @map: see @drm_shadow_plane_state.data
 * @rotation: rotation applied to the source.
 *
 * @src and @dst should have the same size modulo the rotation.
 */
struct castkms_frame_info {
	struct drm_framebuffer *fb;
	struct drm_rect src, dst;
	struct iosys_map map[DRM_FORMAT_MAX_PLANES];
	unsigned int rotation;
};

struct pixel_argb_s32 {
	s32 a, r, g, b;
};

/**
 * struct pixel_argb_u16 - Internal representation of a pixel color.
 * @a: Alpha component value, stored in 16 bits, without padding, using
 *     machine endianness
 * @r: Red component value, stored in 16 bits, without padding, using
 *     machine endianness
 * @g: Green component value, stored in 16 bits, without padding, using
 *     machine endianness
 * @b: Blue component value, stored in 16 bits, without padding, using
 *     machine endianness
 *
 * The goal of this structure is to keep enough precision to ensure
 * correct composition results in CASTKMS and simplifying color
 * manipulation by splitting each component into its own field.
 * Caution: the byte ordering of this structure is machine-dependent,
 * you can't cast it directly to AR48 or xR48.
 */
struct pixel_argb_u16 {
	u16 a, r, g, b;
};

struct line_buffer {
	size_t n_pixels;
	struct pixel_argb_u16 *pixels;
};

/**
 * typedef pixel_write_t - These functions are used to read a pixel from a
 * &struct pixel_argb_u16, convert it in a specific format and write it in the @out_pixel
 * buffer.
 *
 * @out_pixel: destination address to write the pixel
 * @in_pixel: pixel to write
 */
typedef void (*pixel_write_t)(u8 *out_pixel, const struct pixel_argb_u16 *in_pixel);

struct castkms_writeback_job {
	struct iosys_map data[DRM_FORMAT_MAX_PLANES];
	struct castkms_frame_info wb_frame_info;
	pixel_write_t pixel_write;
};

/**
 * enum pixel_read_direction - Enum used internally by CASTKMS to represent a reading direction in a
 * plane.
 */
enum pixel_read_direction {
	READ_BOTTOM_TO_TOP,
	READ_TOP_TO_BOTTOM,
	READ_RIGHT_TO_LEFT,
	READ_LEFT_TO_RIGHT
};

struct castkms_plane_state;

/**
 * typedef pixel_read_line_t - These functions are used to read a pixel line in the source frame,
 * convert it to `struct pixel_argb_u16` and write it to @out_pixel.
 *
 * @plane: plane used as source for the pixel value
 * @x_start: X (width) coordinate of the first pixel to copy. The caller must ensure that x_start
 * is non-negative and smaller than @plane->frame_info->fb->width.
 * @y_start: Y (height) coordinate of the first pixel to copy. The caller must ensure that y_start
 * is non-negative and smaller than @plane->frame_info->fb->height.
 * @direction: direction to use for the copy, starting at @x_start/@y_start
 * @count: number of pixels to copy
 * @out_pixel: pointer where to write the pixel values. They will be written from @out_pixel[0]
 * (included) to @out_pixel[@count] (excluded). The caller must ensure that out_pixel have a
 * length of at least @count.
 */
typedef void (*pixel_read_line_t)(const struct castkms_plane_state *plane, int x_start,
				  int y_start, enum pixel_read_direction direction, int count,
				  struct pixel_argb_u16 out_pixel[]);

/**
 * struct conversion_matrix - Matrix to use for a specific encoding and range
 *
 * @matrix: Conversion matrix from yuv to rgb. The matrix is stored in a row-major manner and is
 * used to compute rgb values from yuv values:
 *     [[r],[g],[b]] = @matrix * [[y],[u],[v]]
 *   OR for yvu formats:
 *     [[r],[g],[b]] = @matrix * [[y],[v],[u]]
 *  The values of the matrix are signed fixed-point values with 32 bits fractional part.
 * @y_offset: Offset to apply on the y value.
 */
struct conversion_matrix {
	s64 matrix[3][3];
	int y_offset;
};

/**
 * struct castkms_plane_state - Driver specific plane state
 * @base: base plane state
 * @frame_info: data required for composing computation
 * @pixel_read_line: function to read a pixel line in this plane. The creator of a
 *		     struct castkms_plane_state must ensure that this pointer is valid
 * @conversion_matrix: matrix used for yuv formats to convert to rgb
 */
struct castkms_plane_state {
	struct drm_shadow_plane_state base;
	struct castkms_frame_info *frame_info;
	pixel_read_line_t pixel_read_line;
	struct conversion_matrix conversion_matrix;
};

struct castkms_plane {
	struct drm_plane base;
};

struct castkms_color_lut {
	const struct drm_color_lut *base;
	size_t lut_length;
	s64 channel_value2index_ratio;
};

/**
 * struct castkms_crtc_state - Driver specific CRTC state
 *
 * @base: base CRTC state
 * @composer_work: work struct to compose and add CRC entries
 *
 * @num_active_planes: Number of active planes
 * @active_planes: List containing all the active planes (counted by
 *		   @num_active_planes). They should be stored in z-order.
 * @active_writeback: Current active writeback job
 * @gamma_lut: Look up table for gamma used in this CRTC
 * @crc_pending: Protected by @castkms_output.composer_lock, true when the frame CRC is not computed
 *		 yet. Used by vblank to detect if the composer is too slow.
 * @wb_pending: Protected by @castkms_output.composer_lock, true when a writeback frame is requested.
 * @frame_start: Protected by @castkms_output.composer_lock, saves the frame number before the start
 *		 of the composition process.
 * @frame_end: Protected by @castkms_output.composer_lock, saves the last requested frame number.
 *	       This is used to generate enough CRC entries when the composition worker is too slow.
 */
struct castkms_crtc_state {
	struct drm_crtc_state base;
	struct work_struct composer_work;

	int num_active_planes;
	struct castkms_plane_state **active_planes;
	struct castkms_writeback_job *active_writeback;
	struct castkms_color_lut gamma_lut;

	bool crc_pending;
	bool wb_pending;
	u64 frame_start;
	u64 frame_end;
};

/**
 * struct castkms_output - Internal representation of all output components in CASTKMS
 *
 * @crtc: Base CRTC in DRM
 * @wb_connector: DRM writeback connector used for this output
 * @wb_encoder: DRM encoder used by @wb_connector
 * @composer_workq: Ordered workqueue for @composer_state.composer_work.
 * @lock: Lock used to protect the current composer state and scheduling
 * @composer_enabled: Protected by @lock, true when the CASTKMS composer is active (crc needed or
 *		      writeback)
 * @composer_state: Protected by @lock, current state of this CASTKMS output
 * @composer_lock: Lock used internally to protect @composer_state members
 */
struct castkms_output {
	struct drm_crtc crtc;
	struct drm_writeback_connector wb_connector;
	struct drm_encoder wb_encoder;
	struct workqueue_struct *composer_workq;
	spinlock_t lock;

	bool composer_enabled;
	struct castkms_crtc_state *composer_state;

	spinlock_t composer_lock;
};

struct castkms_config;
struct castkms_config_plane;

/**
 * struct castkms_device - Description of a CASTKMS device
 *
 * @drm - Base device in DRM
 * @faux_dev - Associated faux device
 * @output - Configuration and sub-components of the CASTKMS device
 * @config: Configuration used in this CASTKMS device. Runtime callbacks must
 *          hold a drm_dev_enter() reference while accessing it because its
 *          configfs owner may release it after unplug.
 */
struct castkms_device {
	struct drm_device drm;
	struct faux_device *faux_dev;
	struct castkms_config *config;
};

/*
 * The following helpers are used to convert a member of a struct into its parent.
 */

#define drm_crtc_to_castkms_output(target) \
	container_of(target, struct castkms_output, crtc)

#define drm_device_to_castkms_device(target) \
	container_of(target, struct castkms_device, drm)

#define to_castkms_crtc_state(target)\
	container_of(target, struct castkms_crtc_state, base)

#define to_castkms_plane_state(target)\
	container_of(target, struct castkms_plane_state, base.base)

/**
 * castkms_create() - Create a device from a configuration
 * @config: Config used to configure the new device
 *
 * A pointer to the created castkms_device is stored in @config
 *
 * Returns:
 * 0 on success or an error.
 */
int castkms_create(struct castkms_config *config);

/**
 * castkms_destroy() - Destroy a device
 * @config: Config from which the device was created
 *
 * The device is completely removed, but the @config is not freed. It can be
 * reused or destroyed with castkms_config_destroy().
 */
void castkms_destroy(struct castkms_config *config);

/**
 * castkms_crtc_init() - Initialize a CRTC for CASTKMS
 * @dev: DRM device associated with the CASTKMS buffer
 * @crtc: uninitialized CRTC device
 * @primary: primary plane to attach to the CRTC
 * @cursor: plane to attach to the CRTC
 */
struct castkms_output *castkms_crtc_init(struct drm_device *dev,
				   struct drm_plane *primary,
				   struct drm_plane *cursor);

/**
 * castkms_output_init() - Initialize all sub-components needed for a CASTKMS device.
 *
 * @castkmsdev: CASTKMS device to initialize
 */
int castkms_output_init(struct castkms_device *castkmsdev);

/**
 * castkms_plane_init() - Initialize a plane
 *
 * @castkmsdev: CASTKMS device containing the plane
 * @plane_cfg: plane configuration
 */
struct castkms_plane *castkms_plane_init(struct castkms_device *castkmsdev,
				   struct castkms_config_plane *plane_cfg);

/* CRC Support */
const char *const *castkms_get_crc_sources(struct drm_crtc *crtc,
					size_t *count);
int castkms_set_crc_source(struct drm_crtc *crtc, const char *src_name);
int castkms_verify_crc_source(struct drm_crtc *crtc, const char *source_name,
			   size_t *values_cnt);

/* Composer Support */
void castkms_composer_worker(struct work_struct *work);
void castkms_set_composer(struct castkms_output *out, bool enabled);
void castkms_writeback_row(struct castkms_writeback_job *wb, const struct line_buffer *src_buffer, int y);

/* Writeback */
int castkms_enable_writeback_connector(struct castkms_device *castkmsdev, struct castkms_output *castkms_out);

/* Colorops */
int castkms_initialize_colorops(struct drm_plane *plane);

#endif /* _CASTKMS_DRV_H_ */
