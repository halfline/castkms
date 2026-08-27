/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CASTKMS_UAPI_DEVICE_H_
#define _CASTKMS_UAPI_DEVICE_H_

#include <linux/container_of.h>

#include "castkms_device.h"

struct castkms_grant_registry;

/**
 * struct castkms_uapi_device - Driver/UAPI shell around a core CastKMS device
 * @core: Transport-neutral device state
 * @grant_registry: Private grant-fd ID namespace owned by castkms_grant.c
 *
 * Only driver assembly and the grant adapter use this shell. Keeping the
 * grant registry here prevents core device consumers from observing even an
 * opaque grant-fd namespace.
 */
struct castkms_uapi_device {
	struct castkms_device core;
	struct castkms_grant_registry *grant_registry;
};

#define castkms_device_to_uapi_device(target) \
	container_of(target, struct castkms_uapi_device, core)

#define drm_device_to_castkms_uapi_device(target) \
	castkms_device_to_uapi_device(drm_device_to_castkms_device(target))

#endif /* _CASTKMS_UAPI_DEVICE_H_ */
