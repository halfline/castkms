/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_CEC_H_
#define _CASTKMS_CEC_H_

#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

struct castkms_connector;
struct castkms_device;
struct drm_connector;
struct drm_device;
struct drm_file;

#if IS_REACHABLE(CONFIG_DRM_DISPLAY_HDMI_CEC_HELPER)

/**
 * struct castkms_cec_output - Per-connector CEC adapter and transport state
 *
 * @connector: Back-pointer to the owning castkms connector
 * @lock: Protects transport ownership, pending transaction, and state fields
 * @transport_file: DRM file that owns the backend binding, or NULL
 * @transport_id: File-local binding identifier
 * @transport_generation: Monotonic; incremented on rebind to reject stale events
 * @transport_online: Whether the backend is ready for dispatch
 * @adapter_enabled: Latest CEC core enable state
 * @logical_addr_mask: Logical addresses assigned by the CEC core
 * @state_generation: Monotonic; incremented on any state change
 * @next_cookie: Monotonic outbound transaction cookie
 * @pending_cookie: Cookie of the outstanding transaction, or 0
 * @pending_msg_len: Length of the pending outbound message
 * @pending_attempts: Requested attempt count for the pending message
 * @pending_signal_free_time: Signal-free time for the pending message
 * @pending_msg: Pending outbound CEC message bytes
 * @tx_timeout_work: Completes abandoned transactions
 * @stats_tx_submitted: Number of transmits delivered to userspace
 * @stats_tx_completed: Number of successful completions
 * @stats_tx_nack: Number of NACK completions
 * @stats_tx_error: Number of error completions
 * @stats_tx_timeout: Number of timeout completions
 * @stats_rx: Number of received messages injected
 * @stats_invalid: Number of rejected invalid requests
 */
struct castkms_cec_output {
	struct castkms_connector *connector;
	spinlock_t lock;

	struct drm_file *transport_file;
	u32 transport_id;
	u64 transport_generation;
	bool transport_online;

	bool adapter_enabled;
	u16 logical_addr_mask;
	u64 state_generation;

	u64 next_cookie;
	u64 pending_cookie;
	u8 pending_msg_len;
	u8 pending_attempts;
	u32 pending_signal_free_time;
	u8 pending_msg[16];
	struct delayed_work tx_timeout_work;

	u64 stats_tx_submitted;
	u64 stats_tx_completed;
	u64 stats_tx_nack;
	u64 stats_tx_error;
	u64 stats_tx_timeout;
	u64 stats_rx;
	u64 stats_invalid;
};

int castkms_cec_init(struct castkms_device *castkmsdev);
void castkms_cec_connector_init(struct castkms_connector *castkms_conn);
void castkms_cec_refresh_connector_state(struct drm_connector *connector);

int castkms_cec_bind_transport(struct drm_device *dev, void *data,
			       struct drm_file *file_priv);
int castkms_cec_unbind_transport(struct drm_device *dev, void *data,
				 struct drm_file *file_priv);
int castkms_cec_set_transport_state(struct drm_device *dev, void *data,
				    struct drm_file *file_priv);
int castkms_cec_tx_complete(struct drm_device *dev, void *data,
			    struct drm_file *file_priv);
int castkms_cec_receive(struct drm_device *dev, void *data,
			struct drm_file *file_priv);
int castkms_cec_query_caps(struct drm_device *dev, void *data,
			   struct drm_file *file_priv);
int castkms_cec_get_state(struct drm_device *dev, void *data,
			  struct drm_file *file_priv);

void castkms_cec_unbind_file(struct drm_device *dev, struct drm_file *file);

#else /* !CONFIG_DRM_DISPLAY_HDMI_CEC_HELPER */

struct castkms_cec_output;

static inline int
castkms_cec_init(struct castkms_device *castkmsdev) { return 0; }

static inline void
castkms_cec_connector_init(struct castkms_connector *castkms_conn) { }

static inline void
castkms_cec_refresh_connector_state(struct drm_connector *connector) { }

static inline void
castkms_cec_unbind_file(struct drm_device *dev, struct drm_file *file) { }

#endif /* CONFIG_DRM_DISPLAY_HDMI_CEC_HELPER */
#endif /* _CASTKMS_CEC_H_ */
