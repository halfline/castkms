/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_DRV_H_
#define _CASTKMS_DRV_H_

#include <linux/hrtimer.h>

#include <drm/drm.h>
#include <drm/drm_colorop.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_encoder.h>
#include <drm/drm_writeback.h>

#include "castkms_frame.h"
#include "castkms_crtc.h"
#include "castkms_plane.h"

#define DEFAULT_DEVICE_NAME "castkms"

#define XRES_MIN    10
#define YRES_MIN    10

#define XRES_DEF  1024
#define YRES_DEF   768

#define XRES_MAX  8192
#define YRES_MAX  8192

#define NUM_OVERLAY_PLANES 8
#define CASTKMS_MAX_OUTPUT_OBJECTS 31

enum castkms_composer_client {
	CASTKMS_COMPOSER_CLIENT_CRC,
	CASTKMS_COMPOSER_CLIENT_WRITEBACK,
};

/**
 * struct castkms_composer_demand - Clients requiring frame composition
 * @crc_enabled: Whether CRC capture requires composition
 * @writeback_count: Number of committed writeback jobs awaiting composition
 */
struct castkms_composer_demand {
	bool crc_enabled;
	unsigned int writeback_count;
};

static inline bool
castkms_composer_demand_is_active(const struct castkms_composer_demand *demand)
{
	return demand->crc_enabled || demand->writeback_count;
}

/**
 * struct castkms_output - Internal representation of all output components in CASTKMS
 *
 * @crtc: Base CRTC in DRM
 * @wb_connector: DRM writeback connector used for this output
 * @wb_encoder: DRM encoder used by @wb_connector
 * @composer_workq: Ordered workqueue for @composer_state.composer_work.
 * @lock: Lock used to protect the current composer state and scheduling
 * @composer_demand: Protected by @lock, clients keeping the composer active
 * @composer_state: Protected by @lock, current state of this CASTKMS output
 * @composer_lock: Lock used internally to protect @composer_state members
 */
struct castkms_output {
	struct drm_crtc crtc;
	struct drm_writeback_connector wb_connector;
	struct drm_encoder wb_encoder;
	struct workqueue_struct *composer_workq;
	spinlock_t lock;

	struct castkms_composer_demand composer_demand;
	struct castkms_crtc_state *composer_state;

	spinlock_t composer_lock;
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
 */
struct castkms_device {
	struct drm_device drm;
	struct faux_device *faux_dev;
	struct castkms_config *config;
};

/*
 * The following helpers are used to convert a member of a struct into its parent.
 */

#define drm_crtc_to_castkms_output(target) \
	container_of(target, struct castkms_output, crtc)

#define drm_device_to_castkms_device(target) \
	container_of(target, struct castkms_device, drm)

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

#if IS_ENABLED(CONFIG_KUNIT)
void castkms_sort_plane_states(struct castkms_plane_state **planes,
			       size_t count);
#endif

#endif /* _CASTKMS_DRV_H_ */
