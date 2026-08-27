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
 * @dispatch_work: Work item that services requested frame consumers
 * @frame: Renderer input produced during atomic check
 * @active_writeback: Current writeback destination buffer
 * @crc_pending: Whether CRC composition is pending
 * @wb_pending: Whether writeback composition is pending
 * @frame_start: Frame number at the start of composition
 * @frame_end: Last requested frame number
 *
 * Pending flags and frame numbers are protected by
 * &castkms_output.dispatch_lock.
 */
struct castkms_crtc_state {
	struct drm_crtc_state base;
	struct work_struct dispatch_work;

	struct castkms_frame_stage frame;
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
