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
 * struct castkms_cec_tx - transport-neutral outbound CEC transaction
 * @transport_generation: Binding generation which owns this transaction
 * @state_generation: CEC state generation at submission
 * @cookie: Transaction identity used for completion
 * @signal_free_time: CEC-core signal-free-time request
 * @attempts: CEC-core attempt count
 * @length: Number of bytes in @msg
 * @msg: CEC message bytes
 */
struct castkms_cec_tx {
	u64 transport_generation;
	u64 state_generation;
	u64 cookie;
	u32 signal_free_time;
	u8 attempts;
	u8 length;
	u8 msg[CASTKMS_CEC_MAX_MSG_SIZE];
};

/**
 * struct castkms_cec_request - prepared transport delivery
 * @complete: Publish or cancel the prepared delivery
 * @retire: Release adapter lifetime state after the core callback-use barrier
 * @retire_data: Opaque state passed to @retire
 *
 * A transport adapter returns a prepared request from ->prepare_tx().  The CEC
 * core calls @complete exactly once.  A cancelled request must not be exposed
 * to the transport consumer.  The request allocation may be consumed by
 * @complete, so the core snapshots @retire and @retire_data first.  It invokes
 * @retire only after leaving the transport-use barrier; this lets a DRM adapter
 * drop an active file reference without deadlocking against final close.
 */
struct castkms_cec_request {
	void (*complete)(struct castkms_cec_request *request, bool cancelled);
	void (*retire)(void *data);
	void *retire_data;
};

/**
 * struct castkms_cec_transport_ops - CEC transport adapter operations
 * @prepare_tx: Prepare delivery of one outbound transaction
 * @release: Release adapter-private state after the binding is removed
 */
struct castkms_cec_transport_ops {
	int (*prepare_tx)(void *data, const struct castkms_cec_tx *tx,
			  struct castkms_cec_request **request);
	void (*release)(void *data);
};

/**
 * struct castkms_cec_state - transport-neutral CEC state snapshot
 * @transport_generation: Current binding generation
 * @state_generation: Generation of the complete state snapshot
 * @pending_cookie: Outstanding outbound transaction, or zero
 * @stats_tx_submitted: Transactions submitted to the transport
 * @stats_tx_completed: Successful transmit completions
 * @stats_tx_nack: NACK transmit completions
 * @stats_tx_error: Other transmit errors
 * @stats_tx_timeout: Timed-out transactions
 * @stats_invalid: Rejected transport requests
 * @phys_addr: Current EDID-derived physical address
 * @logical_addr_mask: Logical addresses assigned by the CEC core
 * @transport_online: Whether the transport is accepting transactions
 * @monitor_attached: Whether the connector currently has a monitor
 * @adapter_enabled: Whether the CEC adapter is enabled
 */
struct castkms_cec_state {
	u64 transport_generation;
	u64 state_generation;
	u64 pending_cookie;
	u64 stats_tx_submitted;
	u64 stats_tx_completed;
	u64 stats_tx_nack;
	u64 stats_tx_error;
	u64 stats_tx_timeout;
	u64 stats_invalid;
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
int castkms_cec_core_set_online(struct castkms_cec_output *output,
				struct castkms_capture_authority *authority,
				u64 transport_generation, bool online);
int castkms_cec_core_tx_complete(struct castkms_cec_output *output,
				 struct castkms_capture_authority *authority,
				 u64 transport_generation, u64 cookie, u8 status,
				 u8 arb_lost_cnt, u8 nack_cnt, u8 low_drive_cnt,
				 u8 error_cnt);
int castkms_cec_core_get_state(struct castkms_cec_output *output,
			       struct castkms_capture_authority *authority,
			       struct castkms_cec_state *state);

#if IS_ENABLED(CONFIG_KUNIT)
/**
 * struct castkms_cec_test_ops - notifications observed by the fake CEC sink
 * @tx_done: A transaction completed, timed out, or was aborted
 */
struct castkms_cec_test_ops {
	void (*tx_done)(void *data, u8 status, u8 arb_lost_cnt, u8 nack_cnt,
			u8 low_drive_cnt, u8 error_cnt);
};

struct castkms_cec_output *
castkms_cec_core_test_output_create(struct castkms_connector *connector,
				    const struct castkms_cec_test_ops *ops,
				    void *data);
void castkms_cec_core_test_output_destroy(struct castkms_cec_output *output);
int castkms_cec_core_test_enable(struct castkms_cec_output *output,
				 bool enable);
int castkms_cec_core_test_transmit(struct castkms_cec_output *output,
				   u8 attempts, u32 signal_free_time,
				   const u8 *message, u8 length);
void castkms_cec_core_test_timeout(struct castkms_cec_output *output);
#endif

#endif /* _CASTKMS_CEC_CORE_H_ */
