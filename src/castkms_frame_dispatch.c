// SPDX-License-Identifier: GPL-2.0+

#include <linux/limits.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include <drm/drm_crtc.h>
#include <drm/drm_print.h>
#include <drm/drm_vblank.h>
#include <drm/drm_writeback.h>

#include <kunit/visibility.h>

#include "castkms_capture_owner.h"
#include "castkms_composer.h"
#include "castkms_crc.h"
#include "castkms_crtc.h"
#include "castkms_frame_dispatch.h"
#include "castkms_output.h"

/**
 * castkms_frame_dispatch_worker - service pending frame consumers
 * @work: CRTC-state work item
 *
 * Coordinates writeback and CRC consumers around the immutable frame stage
 * produced by atomic check. Pixel rendering itself remains in
 * castkms_composer.c.
 */
void castkms_frame_dispatch_worker(struct work_struct *work)
{
	struct castkms_crtc_state *crtc_state = container_of(
		work, struct castkms_crtc_state, dispatch_work);
	struct drm_crtc *crtc = crtc_state->base.crtc;
	struct castkms_output_buffer *active_wb;
	struct castkms_output *out = drm_crtc_to_castkms_output(crtc);
	const struct castkms_frame_stage *frame = &crtc_state->frame;
	bool crc_pending, wb_pending;
	u64 frame_start, frame_end;
	u32 crc32 = 0;
	int ret = 0;

	spin_lock_irq(&out->dispatch_lock);
	frame_start = crtc_state->frame_start;
	frame_end = crtc_state->frame_end;
	crc_pending = crtc_state->crc_pending;
	wb_pending = crtc_state->wb_pending;
	active_wb = crtc_state->active_writeback;
	crtc_state->frame_start = 0;
	crtc_state->frame_end = 0;
	/*
	 * crc_pending is cleared eagerly so the vblank timer can detect a slow
	 * worker and accumulate frame_start/frame_end. The writeback flag also
	 * guards its destination pointer, so it clears after rendering.
	 */
	crtc_state->crc_pending = false;
	spin_unlock_irq(&out->dispatch_lock);

	if (!crc_pending && !wb_pending)
		return;

	if (WARN_ON(wb_pending && !active_wb))
		ret = -EINVAL;
	else if (!ret)
		ret = castkms_compose_targets(
			frame, wb_pending ? active_wb : NULL,
			NULL, &crc32);

	if (wb_pending) {
		int wb_ret = active_wb ? ret : -EINVAL;

		spin_lock_irq(&out->dispatch_lock);
		crtc_state->wb_pending = false;
		crtc_state->active_writeback = NULL;
		spin_unlock_irq(&out->dispatch_lock);
		castkms_frame_dispatch_put(out, CASTKMS_FRAME_DISPATCH_CLIENT_WRITEBACK);
		drm_writeback_signal_completion(&out->wb_connector, wb_ret);
	}

	if (ret || !crc_pending ||
	    !castkms_capture_owner_is_active_current(
		    crtc->dev, crtc_state->capture_owner))
		return;

	while (frame_start <= frame_end)
		drm_crtc_add_crc_entry(crtc, true, frame_start++, &crc32);
}

static const char *const pipe_crc_sources[] = { "auto" };

const char *const *castkms_get_crc_sources(struct drm_crtc *crtc,
					    size_t *count)
{
	(void)crtc;
	*count = ARRAY_SIZE(pipe_crc_sources);
	return pipe_crc_sources;
}

static int castkms_crc_parse_source(const char *source_name, bool *enabled)
{
	if (!source_name) {
		*enabled = false;
		return 0;
	}
	if (!strcmp(source_name, "auto")) {
		*enabled = true;
		return 0;
	}

	*enabled = false;
	return -EINVAL;
}

int castkms_verify_crc_source(struct drm_crtc *crtc,
			      const char *source_name, size_t *values_count)
{
	bool enabled;

	(void)crtc;
	if (castkms_crc_parse_source(source_name, &enabled) < 0) {
		DRM_DEBUG_DRIVER("unknown source %s\n", source_name);
		return -EINVAL;
	}

	*values_count = 1;
	return 0;
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

int castkms_set_crc_source(struct drm_crtc *crtc, const char *source_name)
{
	struct castkms_output *out = drm_crtc_to_castkms_output(crtc);
	bool enabled = false;
	int ret;

	ret = castkms_crc_parse_source(source_name, &enabled);
	if (ret)
		return ret;

	if (enabled) {
		bool content_safe;

		spin_lock_irq(&out->lock);
		content_safe =
			castkms_capture_output_has_safe_content(out);
		spin_unlock_irq(&out->lock);
		if (!content_safe)
			return -EACCES;

		return castkms_frame_dispatch_get(out,
					   CASTKMS_FRAME_DISPATCH_CLIENT_CRC);
	}

	castkms_frame_dispatch_put(out, CASTKMS_FRAME_DISPATCH_CLIENT_CRC);
	return 0;
}
