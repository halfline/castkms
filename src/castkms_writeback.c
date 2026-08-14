// SPDX-License-Identifier: GPL-2.0+

#include <linux/iosys-map.h>

#include <drm/drm_atomic.h>
#include <drm/drm_edid.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_writeback.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>

#include "castkms_drv.h"
#include "castkms_formats.h"

static const struct drm_connector_funcs castkms_wb_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static int castkms_wb_atomic_check(struct drm_connector *connector,
				struct drm_atomic_state *state)
{
	struct drm_connector_state *conn_state =
		drm_atomic_get_new_connector_state(state, connector);
	struct drm_crtc_state *crtc_state;
	struct drm_framebuffer *fb;
	const struct drm_display_mode *mode;
	int ret;

	if (!conn_state->writeback_job || !conn_state->writeback_job->fb)
		return 0;

	if (!conn_state->crtc)
		return -EINVAL;

	crtc_state = drm_atomic_get_crtc_state(state, conn_state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	mode = &crtc_state->mode;

	fb = conn_state->writeback_job->fb;
	if (!castkms_get_pixel_write_function(fb->format->format))
		return -EINVAL;

	if (fb->width != mode->hdisplay || fb->height != mode->vdisplay) {
		DRM_DEBUG_KMS("Invalid framebuffer size %ux%u\n",
			      fb->width, fb->height);
		return -EINVAL;
	}

	ret = drm_atomic_helper_check_wb_connector_state(connector, state);
	if (ret < 0)
		return ret;

	return 0;
}

static int castkms_wb_connector_get_modes(struct drm_connector *connector)
{
	struct drm_device *dev = connector->dev;

	return drm_add_modes_noedid(connector, dev->mode_config.max_width,
				    dev->mode_config.max_height);
}

static int castkms_wb_prepare_job(struct drm_writeback_connector *wb_connector,
			       struct drm_writeback_job *job)
{
	struct castkms_writeback_job *castkmsjob;
	int ret;

	if (!job->fb)
		return 0;

	castkmsjob = kzalloc_obj(*castkmsjob);
	if (!castkmsjob)
		return -ENOMEM;

	ret = drm_gem_fb_vmap(job->fb, castkmsjob->map, NULL);
	if (ret) {
		DRM_ERROR("vmap failed: %d\n", ret);
		goto err_kfree;
	}

	castkmsjob->wb_frame_info.fb = job->fb;
	castkmsjob->wb_frame_info.map = castkmsjob->map;
	drm_framebuffer_get(castkmsjob->wb_frame_info.fb);

	job->priv = castkmsjob;

	return 0;

err_kfree:
	kfree(castkmsjob);
	return ret;
}

static void castkms_wb_cleanup_job(struct drm_writeback_connector *connector,
				struct drm_writeback_job *job)
{
	struct castkms_writeback_job *castkmsjob = job->priv;

	if (!job->fb)
		return;

	drm_gem_fb_vunmap(job->fb, castkmsjob->map);

	drm_framebuffer_put(castkmsjob->wb_frame_info.fb);

	kfree(castkmsjob);
}

static void castkms_wb_atomic_commit(struct drm_connector *conn,
				  struct drm_atomic_state *state)
{
	struct drm_connector_state *connector_state = drm_atomic_get_new_connector_state(state,
									 conn);
	struct castkms_output *output = drm_crtc_to_castkms_output(connector_state->crtc);
	struct drm_writeback_connector *wb_conn = &output->wb_connector;
	struct drm_crtc_state *new_crtc_state =
		drm_atomic_get_new_crtc_state(state, connector_state->crtc);
	struct castkms_crtc_state *crtc_state;
	struct drm_framebuffer *fb = connector_state->writeback_job->fb;
	struct castkms_writeback_job *active_wb;
	struct castkms_frame_info *wb_frame_info;
	u16 crtc_height;
	u16 crtc_width;
	int ret;

	if (WARN_ON(!new_crtc_state)) {
		ret = -EINVAL;
		goto err_complete_job;
	}

	crtc_state = to_castkms_crtc_state(new_crtc_state);
	crtc_height = new_crtc_state->mode.vdisplay;
	crtc_width = new_crtc_state->mode.hdisplay;

	active_wb = connector_state->writeback_job->priv;
	wb_frame_info = &active_wb->wb_frame_info;

	active_wb->pixel_write = castkms_get_pixel_write_function(fb->format->format);
	drm_rect_init(&wb_frame_info->src, 0, 0, crtc_width, crtc_height);
	drm_rect_init(&wb_frame_info->dst, 0, 0, crtc_width, crtc_height);

	ret = castkms_composer_get(output, CASTKMS_COMPOSER_CLIENT_WRITEBACK);
	if (ret)
		goto err_complete_job;

	drm_writeback_queue_job(wb_conn, connector_state);

	spin_lock_irq(&output->composer_lock);
	crtc_state->active_writeback = active_wb;
	crtc_state->wb_pending = true;
	spin_unlock_irq(&output->composer_lock);

	/*
	 * A vblank can race with this connector hook after atomic_flush() has
	 * published the CRTC state. Queue the state directly so that a newer
	 * atomic commit cannot replace it before this job reaches the worker.
	 */
	queue_work(output->composer_workq, &crtc_state->composer_work);

	return;

err_complete_job:
	drm_writeback_queue_job(wb_conn, connector_state);
	drm_writeback_signal_completion(wb_conn, ret);
}

static const struct drm_connector_helper_funcs castkms_wb_conn_helper_funcs = {
	.get_modes = castkms_wb_connector_get_modes,
	.prepare_writeback_job = castkms_wb_prepare_job,
	.cleanup_writeback_job = castkms_wb_cleanup_job,
	.atomic_commit = castkms_wb_atomic_commit,
	.atomic_check = castkms_wb_atomic_check,
};

int castkms_enable_writeback_connector(struct castkms_device *castkmsdev,
				    struct castkms_output *castkms_output)
{
	struct drm_writeback_connector *wb = &castkms_output->wb_connector;
	u32 *formats;
	int num_formats;
	int ret;

	ret = drmm_encoder_init(&castkmsdev->drm, &castkms_output->wb_encoder,
				NULL, DRM_MODE_ENCODER_VIRTUAL, NULL);
	if (ret)
		return ret;
	castkms_output->wb_encoder.possible_crtcs |= drm_crtc_mask(&castkms_output->crtc);
	castkms_output->wb_encoder.possible_clones |=
		drm_encoder_mask(&castkms_output->wb_encoder);

	drm_connector_helper_add(&wb->base, &castkms_wb_conn_helper_funcs);

	num_formats = castkms_writeback_formats_alloc(&formats);
	if (num_formats < 0)
		return num_formats;

	ret = drmm_writeback_connector_init(&castkmsdev->drm, wb,
					    &castkms_wb_connector_funcs,
					    &castkms_output->wb_encoder,
					    formats, num_formats);
	kfree(formats);

	return ret;
}
