/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_OUTPUT_H_
#define _CASTKMS_OUTPUT_H_

#include <linux/container_of.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <drm/drm_crtc.h>
#include <drm/drm_encoder.h>
#include <drm/drm_writeback.h>

#include "castkms_frame_dispatch_demand.h"

struct castkms_crtc_state;
struct workqueue_struct;

enum castkms_composer_client {
	CASTKMS_COMPOSER_CLIENT_CRC,
	CASTKMS_COMPOSER_CLIENT_WRITEBACK,
};

/**
 * struct castkms_composer_demand - Legacy frame consumers
 * @crc_enabled: Whether CRC capture requires composition
 * @writeback_count: Number of writeback jobs awaiting composition
 */
struct castkms_composer_demand {
	bool crc_enabled;
	unsigned int writeback_count;
};

static inline bool castkms_composer_demand_is_active(
	const struct castkms_composer_demand *demand)
{
	return demand->crc_enabled || demand->writeback_count;
}

/**
 * struct castkms_output - CRTC and its frame-consumer state
 * @crtc: Base DRM CRTC
 * @wb_connector: DRM writeback connector for this output
 * @wb_encoder: DRM encoder used by @wb_connector
 * @composer_workq: Legacy ordered composition workqueue
 * @dispatch_workq: Ordered frame-consumer dispatch workqueue
 * @lock: Protects demand, scheduling, and state assignment
 * @composer_demand: Legacy consumers keeping composition active
 * @composer_state: Current state assigned to legacy composition
 * @composer_lock: Protects legacy per-frame pending fields
 * @dispatch_demand: Consumers keeping frame dispatch active
 * @dispatch_state: Current state assigned to the dispatch worker
 * @dispatch_lock: Protects per-frame pending flags and worker fields
 *
 * Lock ordering (outermost first):
 *
 * 1. @lock
 * 2. @dispatch_lock
 */
struct castkms_output {
	struct drm_crtc crtc;
	struct drm_writeback_connector wb_connector;
	struct drm_encoder wb_encoder;
	struct workqueue_struct *composer_workq;
	struct workqueue_struct *dispatch_workq;
	spinlock_t lock; /* Protects commit and vblank state. */

	struct castkms_composer_demand composer_demand;
	struct castkms_crtc_state *composer_state;
	spinlock_t composer_lock;

	struct castkms_frame_dispatch_demand dispatch_demand;
	struct castkms_crtc_state *dispatch_state;

	spinlock_t dispatch_lock; /* Protects pending frame consumers. */
};

#define drm_crtc_to_castkms_output(target) \
	container_of(target, struct castkms_output, crtc)

#endif /* _CASTKMS_OUTPUT_H_ */
