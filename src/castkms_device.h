/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_DEVICE_H_
#define _CASTKMS_DEVICE_H_

#include <linux/container_of.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/xarray.h>

#include <drm/drm_device.h>

#include "castkms_capture_owner.h"

struct castkms_config;
struct castkms_audio;
struct drm_property;
struct faux_device;

/**
 * struct castkms_device - Description of a CASTKMS device
 * @drm: Base DRM device
 * @faux_dev: Associated faux device
 * @config: Configuration used by this device. Runtime callbacks must hold a
 *          drm_dev_enter() reference while accessing it because its configfs
 *          owner may release it after unplug.
 * @authority_registry_lock: Serializes the kernel-native authority registry
 * @authorities: Live capture authorities, including non-UAPI kernel clients
 * @next_authority_id: Internal cyclic authority-registry cursor
 * @authorities_shutdown: Prevents new authorities during device teardown
 * @capture_owners: Device-global DRM ownership facts for composed content
 * @capture_active_prop: Connector property exposing active capture state
 * @audio: Optional device-global HDMI audio presentation state
 * @attach_transition_lock: Serializes complete attachment transitions,
 *                          including EDID, audio, CEC, and hotplug side effects;
 *                          precedes a holder grant lock when both are needed.
 * @attach_lock: Serializes monitor attachment ownership on connectors
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
	struct castkms_audio *audio;
	struct mutex attach_transition_lock; /* Serializes attach transitions. */
	struct mutex attach_lock; /* Protects connector attach ownership. */
};

#define drm_device_to_castkms_device(target) \
	container_of(target, struct castkms_device, drm)

int castkms_create(struct castkms_config *config);
void castkms_destroy(struct castkms_config *config);

#endif /* _CASTKMS_DEVICE_H_ */
