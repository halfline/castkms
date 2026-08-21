/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_CONNECTOR_H_
#define _CASTKMS_CONNECTOR_H_

#include "castkms_drv.h"

struct castkms_cec_output;
struct castkms_capture_authority;
struct drm_crtc;
struct drm_edid;
struct drm_file;

#define drm_connector_to_castkms_connector(target) \
	container_of(target, struct castkms_connector, base)

/**
 * struct castkms_connector - CASTKMS custom type wrapping around the DRM connector
 *
 * @base: Base DRM connector
 * @output_index: Stable non-writeback output identity, assigned once at creation
 * @cec: CEC adapter and transport state, or NULL if CEC is unavailable
 * @attachment_authority: Core authority owning the monitor attachment, or NULL
 * @monitor_attached: Whether ATTACH_MONITOR has plugged a sink into this port
 */
struct castkms_connector {
	struct drm_connector base;
	unsigned int output_index;
	struct castkms_cec_output *cec;
	struct castkms_capture_authority *attachment_authority;
	bool monitor_attached;
};

/**
 * castkms_connector_init() - Initialize a connector
 * @castkmsdev: CASTKMS device containing the connector
 * @output_index: Stable non-writeback output index for this connector
 *
 * Returns:
 * The connector or an error on failure.
 */
struct castkms_connector *castkms_connector_init(struct castkms_device *castkmsdev,
						 unsigned int output_index);

/**
 * castkms_trigger_connector_hotplug() - Update the device's connectors status
 * @castkmsdev: CASTKMS device to update
 */
void castkms_trigger_connector_hotplug(struct castkms_device *castkmsdev);

/*
 * Callers must hold castkms_device.attach_transition_lock and a matching
 * authority acquired with castkms_capture_authority_begin(). Stream teardown
 * for a successful detach runs only after releasing the transition lock.
 */
int castkms_connector_attach_monitor(struct drm_connector *connector,
					     struct castkms_capture_authority *authority,
					     const struct drm_edid *drm_edid);
int castkms_connector_detach_monitor(struct drm_connector *connector,
					     struct castkms_capture_authority *authority);
int castkms_connector_update_authority_edid(struct drm_connector *connector,
					struct castkms_capture_authority *authority,
					const struct drm_edid *drm_edid);
bool castkms_connector_authority_is_attached(
	struct drm_connector *connector,
	struct castkms_capture_authority *authority);
bool castkms_connector_is_attached(struct drm_connector *connector);
bool castkms_connector_is_attached_fast(struct drm_connector *connector);
int castkms_connector_require_authority_attached(
	struct drm_connector *connector,
	struct castkms_capture_authority *authority);
int castkms_connector_get_routed_output(
	struct drm_connector *connector,
	struct castkms_output **output);
bool castkms_connector_detach_authority(
	struct castkms_capture_authority *authority);
void castkms_connector_set_capture_active(struct drm_connector *connector,
					  bool active);

#endif /* _CASTKMS_CONNECTOR_H_ */
