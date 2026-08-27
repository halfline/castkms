/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_OUTPUT_H_
#define _CASTKMS_OUTPUT_H_

#include <linux/container_of.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <drm/drm_crtc.h>
#include <drm/drm_encoder.h>
#include <drm/drm_writeback.h>

#include "castkms_capture_output.h"
#include "castkms_frame_dispatch_demand.h"

struct castkms_crtc_state;
struct drm_master;
struct workqueue_struct;

/**
 * struct castkms_output - CRTC and its frame-consumer state
 * @crtc: Base DRM CRTC
 * @wb_connector: DRM writeback connector for this output
 * @wb_encoder: DRM encoder used by @wb_connector
 * @dispatch_workq: Ordered frame-consumer dispatch workqueue
 * @lock: Protects demand, scheduling, state assignment, and capture ownership
 * @dispatch_demand: Consumers keeping frame dispatch active
 * @dispatch_state: Current state assigned to the dispatch worker
 * @dispatch_lock: Protects per-frame pending flags and worker fields
 * @capture_owner: Refcounted master whose content is safe to expose
 * @capture_owner_generation: Changes when safe ownership is republished
 * @capture_owner_updating: Whether an atomic commit is replacing safe content
 * @capture: Capture stream, buffer, and mode-generation state
 *
 * Lock ordering (outermost first):
 *
 * 1. @lock
 * 2. capture state_lock (in castkms_capture.c)
 * 3. @dispatch_lock
 */
struct castkms_output {
	struct drm_crtc crtc;
	struct drm_writeback_connector wb_connector;
	struct drm_encoder wb_encoder;
	struct workqueue_struct *dispatch_workq;
	spinlock_t lock; /* Protects commit, vblank, and capture-owner state. */

	struct castkms_frame_dispatch_demand dispatch_demand;
	struct castkms_crtc_state *dispatch_state;

	spinlock_t dispatch_lock; /* Protects pending frame consumers. */
	struct drm_master *capture_owner;
	u64 capture_owner_generation;
	bool capture_owner_updating;

	struct castkms_capture_output capture;
};

#define drm_crtc_to_castkms_output(target) \
	container_of(target, struct castkms_output, crtc)

#endif /* _CASTKMS_OUTPUT_H_ */
