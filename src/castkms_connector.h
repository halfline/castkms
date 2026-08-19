/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_CONNECTOR_H_
#define _CASTKMS_CONNECTOR_H_

#include "castkms_drv.h"

struct drm_crtc;
struct drm_edid;
struct drm_file;

#define drm_connector_to_castkms_connector(target) \
	container_of(target, struct castkms_connector, base)

/**
 * struct castkms_connector - CASTKMS custom type wrapping around the DRM connector
 *
 * @base: Base DRM connector
 * @attach_file: File that owns the current monitor attachment, or NULL
 * @monitor_attached: Whether ATTACH_MONITOR has plugged a sink into this port
 */
struct castkms_connector {
	struct drm_connector base;
	struct drm_file *attach_file;
	bool monitor_attached;
};

/**
 * castkms_connector_init() - Initialize a connector
 * @castkmsdev: CASTKMS device containing the connector
 *
 * Returns:
 * The connector or an error on failure.
 */
struct castkms_connector *castkms_connector_init(struct castkms_device *castkmsdev);

/**
 * castkms_trigger_connector_hotplug() - Update the device's connectors status
 * @castkmsdev: CASTKMS device to update
 */
void castkms_trigger_connector_hotplug(struct castkms_device *castkmsdev);

/**
 * castkms_connector_update_edid() - Publish or clear the display connector EDID
 * @crtc: CRTC whose non-writeback connector should be updated
 * @drm_edid: Validated EDID, or NULL to clear
 *
 * Updates the connector EDID property and emits a standard KMS hotplug.
 */
int castkms_connector_update_edid(struct drm_crtc *crtc,
				  const struct drm_edid *drm_edid);
int castkms_connector_attach_monitor(struct drm_connector *connector,
				     struct drm_file *file,
				     const struct drm_edid *drm_edid);
int castkms_connector_detach_monitor(struct drm_connector *connector,
				     struct drm_file *file);
int castkms_connector_require_attached(struct drm_crtc *crtc,
				       struct drm_file *file);
void castkms_connector_set_capture_active(struct drm_crtc *crtc, bool active);
void castkms_connectors_detach_file(struct drm_device *dev,
				    struct drm_file *file);

#endif /* _CASTKMS_CONNECTOR_H_ */
