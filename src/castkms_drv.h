/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_DRV_H_
#define _CASTKMS_DRV_H_

#include <linux/hrtimer.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>

#include <drm/drm.h>
#include <drm/drm_colorop.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_encoder.h>
#include <drm/drm_writeback.h>

#include "castkms_capture.h"

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
 * @map: Borrowed view of raw per-plane buffer-object mappings. Framebuffer
 *       offsets have not been applied, and the mapping owner must outlive this
 *       frame description.
 * @rotation: rotation applied to the source.
 *
 * @src and @dst should have the same size modulo the rotation.
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

enum castkms_composer_client {
	CASTKMS_COMPOSER_CLIENT_CRC,
	CASTKMS_COMPOSER_CLIENT_WRITEBACK,
	CASTKMS_COMPOSER_CLIENT_CAPTURE,
};

/**
 * struct castkms_composer_demand - Clients requiring frame composition
 * @crc_enabled: Whether CRC capture requires composition
 * @writeback_count: Number of committed writeback jobs awaiting composition
 * @capture_count: Number of queued or in-flight capture buffers
 */
struct castkms_composer_demand {
	bool crc_enabled;
	unsigned int writeback_count;
	unsigned int capture_count;
};

static inline bool
castkms_composer_demand_is_active(const struct castkms_composer_demand *demand)
{
	return demand->crc_enabled || demand->writeback_count ||
	       demand->capture_count;
}

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
struct castkms_output_buffer;

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
 * struct castkms_colorop_snapshot - Render state for one color operation
 * @type: Operation selected by the plane's color pipeline
 * @bypass: Whether this operation is disabled
 * @curve_1d_type: Transfer curve selected for a 1D curve operation
 * @has_ctm: Whether @ctm contains matrix data
 * @ctm: Value-owned matrix for a 3x4 CTM operation
 *
 * The composer consumes this value-owned representation instead of following
 * mutable DRM object state after the atomic transaction has been published.
 */
struct castkms_colorop_snapshot {
	enum drm_colorop_type type;
	bool bypass;
	enum drm_colorop_curve_1d_type curve_1d_type;
	bool has_ctm;
	struct drm_color_ctm_3x4 ctm;
};

/**
 * struct castkms_plane_state - Driver specific plane state
 * @base: base plane state
 * @frame_info: data required for composing computation
 * @pixel_read_line: function to read a pixel line in this plane. The creator of a
 *		     struct castkms_plane_state must ensure that this pointer is valid
 * @conversion_matrix: matrix used for yuv formats to convert to rgb
 * @num_colorops: Number of entries in @colorops
 * @colorops: Value-owned color pipeline selected by this plane update
 */
struct castkms_plane_state {
	struct drm_shadow_plane_state base;
	struct castkms_frame_info *frame_info;
	pixel_read_line_t pixel_read_line;
	struct conversion_matrix conversion_matrix;
	size_t num_colorops;
	struct castkms_colorop_snapshot *colorops;
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
 * struct castkms_cursor_snapshot - Cursor state captured at atomic check
 * @visible: Whether the cursor plane is visible in this commit
 * @x: CRTC_X screen position
 * @y: CRTC_Y screen position
 * @hotspot_x: HOTSPOT_X within the cursor image
 * @hotspot_y: HOTSPOT_Y within the cursor image
 * @width: Cursor framebuffer width
 * @height: Cursor framebuffer height
 * @serial: Generation counter; increments when the cursor image, hotspot,
 *          or visibility changes (but not on position-only moves)
 * @fb: Refcounted cursor framebuffer (NULL when invisible)
 */
struct castkms_cursor_snapshot {
	bool visible;
	s32 x, y;
	s32 hotspot_x, hotspot_y;
	u32 width, height;
	u32 serial;
	struct drm_framebuffer *fb;
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
 * @active_writeback: Current active writeback destination buffer
 * @active_capture: Capture destination selected at vblank
 * @gamma_lut: Look up table for gamma used in this CRTC
 * @crc_pending: Protected by @castkms_output.composer_lock, true when the frame CRC is not computed
 *		 yet. Used by vblank to detect if the composer is too slow.
 * @wb_pending: Protected by @castkms_output.composer_lock, true when a writeback frame is requested.
 * @capture_pending: Protected by @castkms_output.composer_lock, true when a
 *                   capture frame is requested.
 * @damage_clip: Bounding box of frame damage in CRTC coordinates, computed
 *               from plane fb_damage_clips during atomic check.
 * @full_damage: True when the damage bounding box covers the entire frame
 *               (plane set changed, mode change, gamma change, etc.).
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
	struct castkms_output_buffer *active_writeback;
	struct castkms_capture_buffer *active_capture;
	struct castkms_color_lut gamma_lut;

	bool crc_pending;
	bool wb_pending;
	bool capture_pending;
	struct drm_rect damage_clip;
	bool full_damage;
	struct castkms_cursor_snapshot cursor;
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
 * @capture_workq: Ordered workqueue for deferred capture composition jobs
 * @lock: Lock used to protect the current composer state and scheduling
 * @composer_demand: Protected by @lock, clients keeping the composer active
 * @composer_state: Protected by @lock, current state of this CASTKMS output
 * @composer_lock: Lock used internally to protect @composer_state members
 * @capture: Passive capture ownership and mode-generation state
 *
 * Lock ordering (outermost first):
 *   1. @lock              — vblank scheduling, composer state assignment
 *   2. capture state_lock — per-stream buffer state machine (in castkms_capture.c)
 *   3. @composer_lock     — per-frame pending flags and worker fields
 */
struct castkms_output {
	struct drm_crtc crtc;
	struct drm_writeback_connector wb_connector;
	struct drm_encoder wb_encoder;
	struct workqueue_struct *composer_workq;
	struct workqueue_struct *capture_workq;
	spinlock_t lock;

	struct castkms_composer_demand composer_demand;
	struct castkms_crtc_state *composer_state;

	spinlock_t composer_lock;

	struct castkms_capture_output capture;
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
 * @attach_lock: Serializes monitor attach/detach ownership on connectors.
 */
struct castkms_device {
	struct drm_device drm;
	struct faux_device *faux_dev;
	struct castkms_config *config;
	struct mutex attach_lock;
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
int castkms_plane_snapshot_colorops(struct castkms_plane_state *plane_state,
				    struct drm_atomic_state *state);

/* CRC Support */
const char *const *castkms_get_crc_sources(struct drm_crtc *crtc,
					size_t *count);
int castkms_set_crc_source(struct drm_crtc *crtc, const char *src_name);
int castkms_verify_crc_source(struct drm_crtc *crtc, const char *source_name,
			   size_t *values_cnt);

/* Composer Support */
void castkms_composer_worker(struct work_struct *work);
int castkms_composer_get(struct castkms_output *out,
			 enum castkms_composer_client client);
void castkms_composer_put(struct castkms_output *out,
			  enum castkms_composer_client client);

/* Writeback */
int castkms_enable_writeback_connector(struct castkms_device *castkmsdev, struct castkms_output *castkms_out);

/* Colorops */
int castkms_initialize_colorops(struct drm_plane *plane);

#if IS_ENABLED(CONFIG_KUNIT)
void castkms_sort_plane_states(struct castkms_plane_state **planes,
			       size_t count);
#endif

#if IS_ENABLED(CONFIG_KUNIT)
int castkms_colorop_snapshot_init(struct castkms_colorop_snapshot *snapshot,
				  const struct drm_colorop *colorop,
				  const struct drm_colorop_state *state);
#endif

#endif /* _CASTKMS_DRV_H_ */
