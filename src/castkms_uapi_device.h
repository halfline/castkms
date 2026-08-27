/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CASTKMS_UAPI_DEVICE_H_
#define _CASTKMS_UAPI_DEVICE_H_

#include <linux/container_of.h>

#include "castkms_drv.h"

/**
 * struct castkms_uapi_device - Driver/UAPI shell around a core CastKMS device
 * @core: Transport-neutral device state
 *
 * Driver assembly owns this outer shell so UAPI-only state need not enter the
 * transport-neutral core device.
 */
struct castkms_uapi_device {
	struct castkms_device core;
};

#define castkms_device_to_uapi_device(target) \
	container_of(target, struct castkms_uapi_device, core)

#define drm_device_to_castkms_uapi_device(target) \
	castkms_device_to_uapi_device(drm_device_to_castkms_device(target))

#endif /* _CASTKMS_UAPI_DEVICE_H_ */
