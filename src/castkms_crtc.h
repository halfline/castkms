/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_CRTC_H_
#define _CASTKMS_CRTC_H_

#include <linux/container_of.h>

#include <drm/drm_crtc.h>

#include "castkms_frame.h"

struct castkms_output;
struct drm_plane;

/**
 * struct castkms_crtc_state - Driver-specific atomic CRTC state
 * @base: Base DRM CRTC state
 * @frame: Renderer input produced during atomic check
 */
struct castkms_crtc_state {
	struct drm_crtc_state base;
	struct castkms_frame_stage frame;
};

#define to_castkms_crtc_state(target) \
	container_of(target, struct castkms_crtc_state, base)

struct castkms_output *castkms_crtc_init(struct drm_device *dev,
					 struct drm_plane *primary,
					 struct drm_plane *cursor);

#endif /* _CASTKMS_CRTC_H_ */
