/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CASTKMS_CEC_UAPI_H_
#define _CASTKMS_CEC_UAPI_H_

struct drm_device;
struct drm_file;

#ifdef CASTKMS_HAVE_CEC

int castkms_cec_query_caps_ioctl(struct drm_device *dev, void *data,
				 struct drm_file *file_priv);
int castkms_cec_bind_transport_ioctl(struct drm_device *dev, void *data,
				     struct drm_file *file_priv);
int castkms_cec_unbind_transport_ioctl(struct drm_device *dev, void *data,
				       struct drm_file *file_priv);
int castkms_cec_set_transport_state_ioctl(struct drm_device *dev, void *data,
					  struct drm_file *file_priv);
int castkms_cec_tx_complete_ioctl(struct drm_device *dev, void *data,
				  struct drm_file *file_priv);
#endif /* CASTKMS_HAVE_CEC */
#endif /* _CASTKMS_CEC_UAPI_H_ */
