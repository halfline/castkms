/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CASTKMS_CONNECTOR_UAPI_H_
#define _CASTKMS_CONNECTOR_UAPI_H_

struct drm_device;
struct drm_file;

int castkms_capture_set_output_edid_ioctl(struct drm_device *dev, void *data,
					  struct drm_file *file_priv);
int castkms_capture_attach_monitor_ioctl(struct drm_device *dev, void *data,
					 struct drm_file *file_priv);

#endif /* _CASTKMS_CONNECTOR_UAPI_H_ */
