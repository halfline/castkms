/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_CEC_CORE_H_
#define _CASTKMS_CEC_CORE_H_

#include <linux/types.h>

struct castkms_cec_output;
struct castkms_connector;
struct drm_connector;
struct drm_device;

#define CASTKMS_CEC_MAX_MSG_SIZE 16

int castkms_cec_core_init(struct drm_device *dev);
int castkms_cec_core_connector_init(struct castkms_connector *connector);
void castkms_cec_core_suspend_connector(struct drm_connector *connector);

#endif /* _CASTKMS_CEC_CORE_H_ */
