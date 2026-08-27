/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_CEC_CORE_H_
#define _CASTKMS_CEC_CORE_H_

#include <linux/types.h>

struct castkms_capture_authority;
struct castkms_cec_output;
struct castkms_connector;
struct drm_connector;
struct drm_device;

#define CASTKMS_CEC_MAX_MSG_SIZE 16

/**
 * struct castkms_cec_transport_ops - CEC transport adapter operations
 * @release: Release adapter-private state after the binding is removed
 */
struct castkms_cec_transport_ops {
	void (*release)(void *data);
};

/**
 * struct castkms_cec_state - transport-neutral CEC state snapshot
 * @transport_generation: Current binding generation
 * @state_generation: Generation of the complete state snapshot
 * @phys_addr: Current EDID-derived physical address
 * @logical_addr_mask: Logical addresses assigned by the CEC core
 * @transport_online: Whether the transport is accepting transactions
 * @monitor_attached: Whether the connector currently has a monitor
 * @adapter_enabled: Whether the CEC adapter is enabled
 */
struct castkms_cec_state {
	u64 transport_generation;
	u64 state_generation;
	u16 phys_addr;
	u16 logical_addr_mask;
	bool transport_online;
	bool monitor_attached;
	bool adapter_enabled;
};

int castkms_cec_core_init(struct drm_device *dev);
int castkms_cec_core_connector_init(struct castkms_connector *connector);
void castkms_cec_core_suspend_connector(struct drm_connector *connector);

int castkms_cec_core_bind(struct castkms_cec_output *output,
			  struct castkms_capture_authority *authority,
			  const struct castkms_cec_transport_ops *ops, void *data,
			  struct castkms_cec_state *state);
int castkms_cec_core_unbind(struct castkms_cec_output *output,
			    struct castkms_capture_authority *authority,
			    u64 transport_generation);
int castkms_cec_core_get_state(struct castkms_cec_output *output,
			       struct castkms_capture_authority *authority,
			       struct castkms_cec_state *state);

#endif /* _CASTKMS_CEC_CORE_H_ */
