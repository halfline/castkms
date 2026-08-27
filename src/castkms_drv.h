/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_DRV_H_
#define _CASTKMS_DRV_H_

#include <linux/hrtimer.h>
#include <linux/mutex.h>
#include <linux/xarray.h>

#include <drm/drm.h>
#include <drm/drm_colorop.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_encoder.h>
#include <drm/drm_writeback.h>

#include "castkms_capture_owner.h"
#include "castkms_limits.h"
#include "castkms_frame.h"
#include "castkms_crtc.h"
#include "castkms_output.h"
#include "castkms_plane.h"

#define DEFAULT_DEVICE_NAME CASTKMS_DEFAULT_DEVICE_NAME

#define XRES_MIN CASTKMS_MIN_WIDTH
#define YRES_MIN CASTKMS_MIN_HEIGHT

#define XRES_DEF CASTKMS_DEFAULT_WIDTH
#define YRES_DEF CASTKMS_DEFAULT_HEIGHT

#define XRES_MAX CASTKMS_MAX_WIDTH
#define YRES_MAX CASTKMS_MAX_HEIGHT

#define NUM_OVERLAY_PLANES CASTKMS_NUM_OVERLAY_PLANES

struct castkms_config;
struct castkms_config_plane;
struct drm_property;

/**
 * struct castkms_device - Description of a CASTKMS device
 *
 * @drm - Base device in DRM
 * @faux_dev - Associated faux device
 * @output - Configuration and sub-components of the CASTKMS device
 * @config: Configuration used in this CASTKMS device. Runtime callbacks must
 *          hold a drm_dev_enter() reference while accessing it because its
 *          configfs owner may release it after unplug.
 * @authority_registry_lock: Serializes the kernel-native authority registry
 * @authorities: Live capture authorities, including non-UAPI kernel clients
 * @next_authority_id: Internal cyclic authority-registry cursor
 * @authorities_shutdown: Prevents new authorities during device teardown
 * @capture_owners: Device-global DRM ownership facts for composed content
 * @capture_active_prop: Connector property exposing active capture state
 * @attach_transition_lock: Serializes complete monitor state transitions
 * @attach_lock: Protects connector attachment state and ownership
 */
struct castkms_device {
	struct drm_device drm;
	struct faux_device *faux_dev;
	struct castkms_config *config;
	struct mutex authority_registry_lock; /* Protects authorities. */
	struct xarray authorities;
	u32 next_authority_id;
	bool authorities_shutdown;
	struct castkms_capture_owner_state capture_owners;
	struct drm_property *capture_active_prop;
	struct mutex attach_transition_lock; /* Serializes attach transitions. */
	struct mutex attach_lock; /* Protects connector attach ownership. */
};

/*
 * The following helpers are used to convert a member of a struct into its parent.
 */

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

#if IS_ENABLED(CONFIG_KUNIT)
void castkms_sort_plane_states(struct castkms_plane_state **planes,
			       size_t count);
#endif

#endif /* _CASTKMS_DRV_H_ */
