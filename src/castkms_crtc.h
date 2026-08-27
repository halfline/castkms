/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_CRTC_H_
#define _CASTKMS_CRTC_H_

#include <linux/container_of.h>
#include <linux/kconfig.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include <drm/drm_crtc.h>

#include "castkms_frame.h"

struct castkms_output;
struct castkms_output_buffer;
struct drm_plane;

/**
 * struct castkms_crtc_state - Driver-specific atomic CRTC state
 * @base: Base DRM CRTC state
 * @frame: Renderer input produced during atomic check
 * @composer_work: Legacy composition work pending scheduler migration
 * @active_writeback: Current writeback destination
 * @crc_pending: Whether a checksum is waiting for composition
 * @wb_pending: Whether a writeback job is waiting for composition
 * @frame_start: First pending vertical-blank sequence
 * @frame_end: Last pending vertical-blank sequence
 */
struct castkms_crtc_state {
	struct drm_crtc_state base;
	struct castkms_frame_stage frame;
	struct work_struct composer_work;
	struct castkms_output_buffer *active_writeback;
	bool crc_pending;
	bool wb_pending;
	u64 frame_start;
	u64 frame_end;
};

#define to_castkms_crtc_state(target) \
	container_of(target, struct castkms_crtc_state, base)

struct castkms_output *castkms_crtc_init(struct drm_device *dev,
					 struct drm_plane *primary,
					 struct drm_plane *cursor);

#if IS_ENABLED(CONFIG_KUNIT)
void castkms_sort_frame_planes(struct castkms_frame_plane **planes,
			       size_t count);
#endif

#endif /* _CASTKMS_CRTC_H_ */
