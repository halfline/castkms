/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_CRC_H_
#define _CASTKMS_CRC_H_

#include <linux/types.h>

struct drm_crtc;

bool castkms_crc_enabled(void);
const char *const *castkms_get_crc_sources(struct drm_crtc *crtc,
					   size_t *count);
int castkms_set_crc_source(struct drm_crtc *crtc, const char *source_name);
int castkms_verify_crc_source(struct drm_crtc *crtc, const char *source_name,
			      size_t *values_count);

#endif /* _CASTKMS_CRC_H_ */
