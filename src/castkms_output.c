// SPDX-License-Identifier: GPL-2.0+

#include "castkms_config.h"
#include "castkms_connector.h"
#include "castkms_drv.h"
#include <drm/drm_managed.h>
#include <drm/drm_print.h>

int castkms_output_init(struct castkms_device *castkmsdev)
{
	struct drm_device *dev = &castkmsdev->drm;
	struct castkms_config_plane *plane_cfg;
	struct castkms_config_crtc *crtc_cfg;
	struct castkms_config_encoder *encoder_cfg;
	struct castkms_config_connector *connector_cfg;
	int ret;
	int writeback;

	if (!castkms_config_is_valid(castkmsdev->config))
		return -EINVAL;

	castkms_config_for_each_plane(castkmsdev->config, plane_cfg) {
		plane_cfg->plane = castkms_plane_init(castkmsdev, plane_cfg);
		if (IS_ERR(plane_cfg->plane)) {
			DRM_DEV_ERROR(dev->dev, "Failed to init castkms plane\n");
			return PTR_ERR(plane_cfg->plane);
		}
	}

	castkms_config_for_each_crtc(castkmsdev->config, crtc_cfg) {
		struct castkms_config_plane *primary, *cursor;

		primary = castkms_config_crtc_primary_plane(castkmsdev->config, crtc_cfg);
		cursor = castkms_config_crtc_cursor_plane(castkmsdev->config, crtc_cfg);

		crtc_cfg->crtc = castkms_crtc_init(dev, &primary->plane->base,
						cursor ? &cursor->plane->base : NULL);
		if (IS_ERR(crtc_cfg->crtc)) {
			DRM_ERROR("Failed to allocate CRTC\n");
			return PTR_ERR(crtc_cfg->crtc);
		}

		/* Initialize the writeback component */
		if (castkms_config_crtc_get_writeback(crtc_cfg)) {
			writeback = castkms_enable_writeback_connector(castkmsdev, crtc_cfg->crtc);
			if (writeback)
				DRM_ERROR("Failed to init writeback connector\n");
		}
	}

	castkms_config_for_each_plane(castkmsdev->config, plane_cfg) {
		struct castkms_config_crtc *possible_crtc;
		unsigned long idx = 0;

		castkms_config_plane_for_each_possible_crtc(plane_cfg, idx, possible_crtc) {
			plane_cfg->plane->base.possible_crtcs |=
				drm_crtc_mask(&possible_crtc->crtc->crtc);
		}
	}

	castkms_config_for_each_encoder(castkmsdev->config, encoder_cfg) {
		struct castkms_config_crtc *possible_crtc;
		unsigned long idx = 0;

		encoder_cfg->encoder = drmm_kzalloc(dev, sizeof(*encoder_cfg->encoder), GFP_KERNEL);
		if (!encoder_cfg->encoder) {
			DRM_ERROR("Failed to allocate encoder\n");
			return -ENOMEM;
		}
		ret = drmm_encoder_init(dev, encoder_cfg->encoder, NULL,
					DRM_MODE_ENCODER_VIRTUAL, NULL);
		if (ret) {
			DRM_ERROR("Failed to init encoder\n");
			return ret;
		}

		encoder_cfg->encoder->possible_clones |=
			drm_encoder_mask(encoder_cfg->encoder);

		castkms_config_encoder_for_each_possible_crtc(encoder_cfg, idx, possible_crtc) {
			encoder_cfg->encoder->possible_crtcs |=
				drm_crtc_mask(&possible_crtc->crtc->crtc);

			if (castkms_config_crtc_get_writeback(possible_crtc)) {
				struct drm_encoder *wb_encoder =
					&possible_crtc->crtc->wb_encoder;

				encoder_cfg->encoder->possible_clones |=
					drm_encoder_mask(wb_encoder);
				wb_encoder->possible_clones |=
					drm_encoder_mask(encoder_cfg->encoder);
			}
		}
	}

	castkms_config_for_each_connector(castkmsdev->config, connector_cfg) {
		struct castkms_config_encoder *possible_encoder;
		unsigned long idx = 0;

		connector_cfg->connector = castkms_connector_init(castkmsdev);
		if (IS_ERR(connector_cfg->connector)) {
			DRM_ERROR("Failed to init connector\n");
			return PTR_ERR(connector_cfg->connector);
		}

		castkms_config_connector_for_each_possible_encoder(connector_cfg,
								idx,
								possible_encoder) {
			ret = drm_connector_attach_encoder(&connector_cfg->connector->base,
							   possible_encoder->encoder);
			if (ret) {
				DRM_ERROR("Failed to attach connector to encoder\n");
				return ret;
			}
		}
	}

	drm_mode_config_reset(dev);

	return 0;
}
