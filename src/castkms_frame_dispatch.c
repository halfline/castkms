// SPDX-License-Identifier: GPL-2.0+

#include <linux/limits.h>
#include <linux/workqueue.h>

#include <drm/drm_crtc.h>
#include <drm/drm_print.h>
#include <drm/drm_vblank.h>

#include <kunit/visibility.h>

#include "castkms_composer.h"
#include "castkms_crtc.h"
#include "castkms_frame_dispatch.h"
#include "castkms_output.h"

/**
 * castkms_frame_dispatch_worker - service pending frame consumers
 * @work: CRTC-state work item
 *
 * Coordinates the CRC consumer around the immutable frame stage produced by
 * atomic check. Pixel rendering itself remains in castkms_composer.c.
 */
void castkms_frame_dispatch_worker(struct work_struct *work)
{
	struct castkms_crtc_state *crtc_state = container_of(
		work, struct castkms_crtc_state, dispatch_work);
	struct drm_crtc *crtc = crtc_state->base.crtc;
	struct castkms_output *out = drm_crtc_to_castkms_output(crtc);
	const struct castkms_frame_stage *frame = &crtc_state->frame;
	bool crc_pending;
	u64 frame_start, frame_end;
	u32 crc32 = 0;
	int ret;

	spin_lock_irq(&out->dispatch_lock);
	frame_start = crtc_state->frame_start;
	frame_end = crtc_state->frame_end;
	crc_pending = crtc_state->crc_pending;
	crtc_state->frame_start = 0;
	crtc_state->frame_end = 0;
	/*
	 * crc_pending is cleared eagerly so the vblank timer can detect a slow
	 * worker and accumulate frame_start/frame_end.
	 */
	crtc_state->crc_pending = false;
	spin_unlock_irq(&out->dispatch_lock);

	if (!crc_pending)
		return;

	ret = castkms_compose_targets(frame, NULL, NULL, &crc32);
	if (ret)
		return;

	while (frame_start <= frame_end)
		drm_crtc_add_crc_entry(crtc, true, frame_start++, &crc32);
}

VISIBLE_IF_KUNIT int
castkms_frame_dispatch_demand_get(
	struct castkms_frame_dispatch_demand *demand,
	enum castkms_frame_dispatch_client client,
	int vblank_ret, bool *keep_vblank)
{
	bool was_active = castkms_frame_dispatch_demand_is_active(demand);

	*keep_vblank = false;
	if (vblank_ret)
		return vblank_ret;

	switch (client) {
	case CASTKMS_FRAME_DISPATCH_CLIENT_CRC:
		if (demand->crc_enabled)
			return 0;
		demand->crc_enabled = true;
		break;
	case CASTKMS_FRAME_DISPATCH_CLIENT_WRITEBACK:
		if (WARN_ON(demand->writeback_count == UINT_MAX))
			return -EOVERFLOW;
		demand->writeback_count++;
		break;
	default:
		WARN_ON(1);
		return -EINVAL;
	}

	*keep_vblank = !was_active;
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(castkms_frame_dispatch_demand_get);

VISIBLE_IF_KUNIT bool
castkms_frame_dispatch_demand_put(
	struct castkms_frame_dispatch_demand *demand,
	enum castkms_frame_dispatch_client client)
{
	bool was_active = castkms_frame_dispatch_demand_is_active(demand);

	switch (client) {
	case CASTKMS_FRAME_DISPATCH_CLIENT_CRC:
		if (!demand->crc_enabled)
			return false;
		demand->crc_enabled = false;
		break;
	case CASTKMS_FRAME_DISPATCH_CLIENT_WRITEBACK:
		if (WARN_ON(!demand->writeback_count))
			return false;
		demand->writeback_count--;
		break;
	default:
		WARN_ON(1);
		return false;
	}

	return was_active && !castkms_frame_dispatch_demand_is_active(demand);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_frame_dispatch_demand_put);

int castkms_frame_dispatch_get(struct castkms_output *out,
			       enum castkms_frame_dispatch_client client)
{
	bool keep_vblank;
	int ret, vblank_ret;

	/*
	 * Get optimistically so vblank setup never runs under out->lock. The
	 * demand state decides whether this becomes the aggregate reference.
	 */
	vblank_ret = drm_crtc_vblank_get(&out->crtc);

	spin_lock_irq(&out->lock);
	ret = castkms_frame_dispatch_demand_get(&out->dispatch_demand, client,
					       vblank_ret, &keep_vblank);
	spin_unlock_irq(&out->lock);

	if (!vblank_ret && !keep_vblank)
		drm_crtc_vblank_put(&out->crtc);

	return ret;
}

void castkms_frame_dispatch_put(struct castkms_output *out,
				enum castkms_frame_dispatch_client client)
{
	bool put_vblank;

	spin_lock_irq(&out->lock);
	put_vblank = castkms_frame_dispatch_demand_put(&out->dispatch_demand,
						      client);
	spin_unlock_irq(&out->lock);

	if (put_vblank)
		drm_crtc_vblank_put(&out->crtc);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_frame_dispatch_put);
