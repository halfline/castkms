// SPDX-License-Identifier: GPL-2.0-only

#include <drm/castkms_drm.h>
#include <drm/drm_mode.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "virtualscreen-edid.h"

/* ABI size checks */
static_assert(sizeof(struct drm_castkms_cec_query_caps) == 40,
	      "cec query caps ABI size changed");
static_assert(sizeof(struct drm_castkms_cec_bind_transport) == 48,
	      "cec bind transport ABI size changed");
static_assert(sizeof(struct drm_castkms_cec_unbind_transport) == 16,
	      "cec unbind transport ABI size changed");
static_assert(sizeof(struct drm_castkms_cec_set_transport_state) == 16,
	      "cec set transport state ABI size changed");
static_assert(sizeof(struct drm_castkms_cec_tx_complete) == 32,
	      "cec tx complete ABI size changed");
static_assert(sizeof(struct drm_castkms_cec_receive) == 40,
	      "cec receive ABI size changed");
static_assert(sizeof(struct drm_castkms_cec_get_state) == 112,
	      "cec get state ABI size changed");
static_assert(sizeof(struct drm_castkms_cec_event_tx) == 72,
	      "cec event tx ABI size changed");

static int tests_run;
static int tests_pass;

#define PASS(name) do { tests_run++; tests_pass++; \
	printf("%-55s pass\n", name); } while (0)
#define FAIL(name, ...) do { tests_run++; \
	printf("%-55s FAIL: ", name); printf(__VA_ARGS__); printf("\n"); } while (0)

static int check_driver_name(int fd)
{
	struct drm_version version = {};
	char name[32] = {};

	version.name = name;
	version.name_len = sizeof(name) - 1;
	if (ioctl(fd, DRM_IOCTL_VERSION, &version) < 0)
		return -1;

	if (version.name_len != strlen("castkms") ||
	    memcmp(name, "castkms", strlen("castkms")))
		return -1;

	return 0;
}

static int open_device(const char *path)
{
	int fd;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open DRM device");
		return -1;
	}

	if (check_driver_name(fd)) {
		close(fd);
		return -1;
	}

	if (ioctl(fd, DRM_IOCTL_DROP_MASTER, 0) < 0 && errno != EINVAL) {
		close(fd);
		return -1;
	}

	return fd;
}

static int find_first_display_connector(int fd, uint32_t *out_id)
{
	uint32_t connector_ids[32];
	struct drm_mode_card_res res = {
		.count_connectors = 32,
		.connector_id_ptr = (uint64_t)(uintptr_t)connector_ids,
	};

	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
		return -1;
	if (!res.count_connectors || res.count_connectors > 32)
		return -1;

	for (uint32_t i = 0; i < res.count_connectors; i++) {
		struct drm_mode_get_connector conn = {
			.connector_id = connector_ids[i],
		};
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
			continue;
		if (conn.connector_type == 16) /* DRM_MODE_CONNECTOR_WRITEBACK */
			continue;
		*out_id = connector_ids[i];
		return 0;
	}

	return -1;
}

static int attach_monitor_cec(int fd, uint32_t connector_id,
			      uint8_t phys_addr_ab, uint8_t phys_addr_cd)
{
	uint8_t edid[CASTKMS_EDID_BLOCK * 2];
	int edid_size;

	edid_size = castkms_fill_edid_full(edid, sizeof(edid), NULL,
					   CASTKMS_EDID_FLAG_AUDIO |
					   CASTKMS_EDID_FLAG_CEC,
					   phys_addr_ab, phys_addr_cd);
	if (edid_size < 0)
		return -1;

	struct drm_castkms_capture_attach_monitor args = {
		.connector_id = connector_id,
		.edid_size = edid_size,
		.edid_ptr = (uint64_t)(uintptr_t)edid,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_ATTACH_MONITOR, &args) < 0)
		return -errno;

	return 0;
}

static int detach_monitor(int fd, uint32_t connector_id)
{
	struct drm_castkms_capture_detach_monitor args = {
		.connector_id = connector_id,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_DETACH_MONITOR, &args) < 0)
		return -errno;

	return 0;
}

static int cec_query_caps(int fd, uint32_t connector_id,
			  struct drm_castkms_cec_query_caps *caps)
{
	*caps = (struct drm_castkms_cec_query_caps){
		.connector_id = connector_id,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CEC_QUERY_CAPS, caps) < 0)
		return -errno;

	return 0;
}

static int cec_bind(int fd, uint32_t connector_id,
		    struct drm_castkms_cec_bind_transport *bind)
{
	*bind = (struct drm_castkms_cec_bind_transport){
		.connector_id = connector_id,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CEC_BIND_TRANSPORT, bind) < 0)
		return -errno;

	return 0;
}

static int cec_unbind(int fd, uint32_t connector_id, uint32_t transport_id)
{
	struct drm_castkms_cec_unbind_transport args = {
		.connector_id = connector_id,
		.transport_id = transport_id,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CEC_UNBIND_TRANSPORT, &args) < 0)
		return -errno;

	return 0;
}

static int cec_set_online(int fd, uint32_t connector_id, uint32_t transport_id,
			  bool online)
{
	struct drm_castkms_cec_set_transport_state args = {
		.connector_id = connector_id,
		.transport_id = transport_id,
		.flags = online ? DRM_CASTKMS_CEC_TRANSPORT_ONLINE : 0,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CEC_SET_TRANSPORT_STATE, &args) < 0)
		return -errno;

	return 0;
}

static int cec_get_state(int fd, uint32_t connector_id, uint32_t transport_id,
			 struct drm_castkms_cec_get_state *state)
{
	*state = (struct drm_castkms_cec_get_state){
		.connector_id = connector_id,
		.transport_id = transport_id,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CEC_GET_STATE, state) < 0)
		return -errno;

	return 0;
}

static int cec_tx_complete(int fd, uint32_t connector_id,
			   uint32_t transport_id, uint64_t generation,
			   uint64_t cookie, uint8_t status)
{
	struct drm_castkms_cec_tx_complete args = {
		.connector_id = connector_id,
		.transport_id = transport_id,
		.transport_generation = generation,
		.cookie = cookie,
		.status = status,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CEC_TX_COMPLETE, &args) < 0)
		return -errno;

	return 0;
}

static int cec_receive(int fd, uint32_t connector_id,
		       uint32_t transport_id, uint64_t generation,
		       const uint8_t *msg, uint8_t length)
{
	struct drm_castkms_cec_receive args = {
		.connector_id = connector_id,
		.transport_id = transport_id,
		.transport_generation = generation,
		.length = length,
	};
	if (length <= sizeof(args.msg))
		memcpy(args.msg, msg, length);

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CEC_RECEIVE, &args) < 0)
		return -errno;

	return 0;
}

/* --- Tests --- */

static void test_query_caps(int fd, uint32_t connector_id)
{
	struct drm_castkms_cec_query_caps caps;
	int ret;

	ret = cec_query_caps(fd, connector_id, &caps);
	if (ret) {
		FAIL("query_caps", "ioctl failed: %s", strerror(-ret));
		return;
	}

	if (caps.uapi_major != DRM_CASTKMS_CEC_UAPI_MAJOR ||
	    caps.uapi_minor != DRM_CASTKMS_CEC_UAPI_MINOR) {
		FAIL("query_caps_version", "got %u.%u, expected %u.%u",
		     caps.uapi_major, caps.uapi_minor,
		     DRM_CASTKMS_CEC_UAPI_MAJOR, DRM_CASTKMS_CEC_UAPI_MINOR);
		return;
	}
	PASS("query_caps_version");

	if (!caps.has_adapter) {
		FAIL("query_caps_adapter", "no adapter registered");
		return;
	}
	PASS("query_caps_adapter");

	if (caps.max_msg_size != 16) {
		FAIL("query_caps_msg_size", "expected 16, got %u",
		     caps.max_msg_size);
		return;
	}
	PASS("query_caps_msg_size");

	uint64_t expected_caps = DRM_CASTKMS_CEC_CAP_ASYNC_TX |
				 DRM_CASTKMS_CEC_CAP_RX_INJECT |
				 DRM_CASTKMS_CEC_CAP_TRANSPORT_STATE |
				 DRM_CASTKMS_CEC_CAP_EDID_PHYS_ADDR;
	if (caps.capabilities != expected_caps) {
		FAIL("query_caps_capabilities",
		     "got 0x%llx, expected 0x%llx",
		     (unsigned long long)caps.capabilities,
		     (unsigned long long)expected_caps);
		return;
	}
	PASS("query_caps_capabilities");
}

static void test_query_caps_bad_connector(int fd)
{
	struct drm_castkms_cec_query_caps caps = {
		.connector_id = 0xdeadbeef,
	};

	int ret = ioctl(fd, DRM_IOCTL_CASTKMS_CEC_QUERY_CAPS, &caps);
	if (ret == 0) {
		FAIL("query_caps_bad_connector", "expected failure");
		return;
	}
	if (errno != ENOENT) {
		FAIL("query_caps_bad_connector",
		     "expected ENOENT, got %s", strerror(errno));
		return;
	}
	PASS("query_caps_bad_connector");
}

static void test_query_caps_reserved_reject(int fd, uint32_t connector_id)
{
	struct drm_castkms_cec_query_caps caps = {
		.connector_id = connector_id,
		.reserved = 1,
	};

	int ret = ioctl(fd, DRM_IOCTL_CASTKMS_CEC_QUERY_CAPS, &caps);
	if (ret == 0) {
		FAIL("query_caps_reserved_reject", "expected failure");
		return;
	}
	if (errno != EINVAL) {
		FAIL("query_caps_reserved_reject",
		     "expected EINVAL, got %s", strerror(errno));
		return;
	}
	PASS("query_caps_reserved_reject");
}

static void test_bind_unbind(int fd, uint32_t connector_id)
{
	struct drm_castkms_cec_bind_transport bind;
	int ret;

	ret = cec_bind(fd, connector_id, &bind);
	if (ret) {
		FAIL("bind_transport", "ioctl failed: %s", strerror(-ret));
		return;
	}

	if (!bind.transport_id) {
		FAIL("bind_transport_id", "got zero transport_id");
		return;
	}
	PASS("bind_transport_id");

	if (!bind.transport_generation) {
		FAIL("bind_transport_generation", "got zero generation");
		return;
	}
	PASS("bind_transport_generation");

	if (bind.state_flags & DRM_CASTKMS_CEC_STATE_TRANSPORT_ONLINE) {
		FAIL("bind_initial_offline",
		     "transport should be offline after bind");
		cec_unbind(fd, connector_id, bind.transport_id);
		return;
	}
	PASS("bind_initial_offline");

	ret = cec_unbind(fd, connector_id, bind.transport_id);
	if (ret) {
		FAIL("unbind_transport", "ioctl failed: %s", strerror(-ret));
		return;
	}
	PASS("unbind_transport");
}

static void test_double_bind(int fd, uint32_t connector_id)
{
	struct drm_castkms_cec_bind_transport bind1, bind2;
	int ret;

	ret = cec_bind(fd, connector_id, &bind1);
	if (ret) {
		FAIL("double_bind_first", "ioctl failed: %s", strerror(-ret));
		return;
	}

	ret = cec_bind(fd, connector_id, &bind2);
	if (ret != -EBUSY) {
		FAIL("double_bind_reject",
		     "expected EBUSY, got %s",
		     ret ? strerror(-ret) : "success");
		cec_unbind(fd, connector_id, bind1.transport_id);
		return;
	}
	PASS("double_bind_reject");

	cec_unbind(fd, connector_id, bind1.transport_id);
}

static void test_unbind_wrong_owner(int fd, uint32_t connector_id)
{
	struct drm_castkms_cec_bind_transport bind;
	int ret;

	ret = cec_bind(fd, connector_id, &bind);
	if (ret) {
		FAIL("unbind_wrong_owner", "bind failed: %s", strerror(-ret));
		return;
	}

	ret = cec_unbind(fd, connector_id, bind.transport_id + 1);
	if (ret != -EACCES) {
		FAIL("unbind_wrong_owner",
		     "expected EACCES, got %s",
		     ret ? strerror(-ret) : "success");
	} else {
		PASS("unbind_wrong_owner");
	}

	cec_unbind(fd, connector_id, bind.transport_id);
}

static void test_set_transport_online(int fd, uint32_t connector_id)
{
	struct drm_castkms_cec_bind_transport bind;
	struct drm_castkms_cec_get_state state;
	int ret;

	ret = cec_bind(fd, connector_id, &bind);
	if (ret) {
		FAIL("set_online", "bind failed: %s", strerror(-ret));
		return;
	}

	ret = cec_set_online(fd, connector_id, bind.transport_id, true);
	if (ret) {
		FAIL("set_online", "ioctl failed: %s", strerror(-ret));
		cec_unbind(fd, connector_id, bind.transport_id);
		return;
	}

	ret = cec_get_state(fd, connector_id, bind.transport_id, &state);
	if (ret) {
		FAIL("set_online_verify", "get_state failed: %s",
		     strerror(-ret));
		cec_unbind(fd, connector_id, bind.transport_id);
		return;
	}

	if (!(state.state_flags & DRM_CASTKMS_CEC_STATE_TRANSPORT_ONLINE)) {
		FAIL("set_online_verify", "transport not online after set");
		cec_unbind(fd, connector_id, bind.transport_id);
		return;
	}
	PASS("set_online_verify");

	ret = cec_set_online(fd, connector_id, bind.transport_id, false);
	if (ret) {
		FAIL("set_offline", "ioctl failed: %s", strerror(-ret));
		cec_unbind(fd, connector_id, bind.transport_id);
		return;
	}

	ret = cec_get_state(fd, connector_id, bind.transport_id, &state);
	if (ret) {
		FAIL("set_offline_verify", "get_state failed: %s",
		     strerror(-ret));
		cec_unbind(fd, connector_id, bind.transport_id);
		return;
	}

	if (state.state_flags & DRM_CASTKMS_CEC_STATE_TRANSPORT_ONLINE) {
		FAIL("set_offline_verify", "transport still online after clear");
	} else {
		PASS("set_offline_verify");
	}

	cec_unbind(fd, connector_id, bind.transport_id);
}

static void test_get_state_monitor_flag(int fd, uint32_t connector_id)
{
	struct drm_castkms_cec_bind_transport bind;
	struct drm_castkms_cec_get_state state;
	int ret;

	ret = cec_bind(fd, connector_id, &bind);
	if (ret) {
		FAIL("get_state_monitor", "bind failed: %s", strerror(-ret));
		return;
	}

	if (!(bind.state_flags & DRM_CASTKMS_CEC_STATE_MONITOR_ATTACHED)) {
		FAIL("get_state_monitor_attached",
		     "monitor should be attached");
		cec_unbind(fd, connector_id, bind.transport_id);
		return;
	}
	PASS("get_state_monitor_attached");

	ret = cec_get_state(fd, connector_id, bind.transport_id, &state);
	if (ret) {
		FAIL("get_state_monitor_flag", "get_state failed: %s",
		     strerror(-ret));
		cec_unbind(fd, connector_id, bind.transport_id);
		return;
	}

	if (!(state.state_flags & DRM_CASTKMS_CEC_STATE_MONITOR_ATTACHED)) {
		FAIL("get_state_monitor_flag",
		     "monitor not flagged in state");
	} else {
		PASS("get_state_monitor_flag");
	}

	cec_unbind(fd, connector_id, bind.transport_id);
}

static void test_get_state_stats(int fd, uint32_t connector_id)
{
	struct drm_castkms_cec_bind_transport bind;
	struct drm_castkms_cec_get_state state;
	int ret;

	ret = cec_bind(fd, connector_id, &bind);
	if (ret) {
		FAIL("get_state_stats", "bind failed: %s", strerror(-ret));
		return;
	}

	ret = cec_get_state(fd, connector_id, bind.transport_id, &state);
	if (ret) {
		FAIL("get_state_stats", "get_state failed: %s",
		     strerror(-ret));
		cec_unbind(fd, connector_id, bind.transport_id);
		return;
	}

	if (state.stats_tx_submitted != 0 || state.stats_tx_completed != 0 ||
	    state.stats_rx != 0 || state.stats_invalid != 0) {
		FAIL("get_state_stats_zero",
		     "expected zero stats after fresh bind");
	} else {
		PASS("get_state_stats_zero");
	}

	cec_unbind(fd, connector_id, bind.transport_id);
}

static void test_get_state_phys_addr(int fd, uint32_t connector_id)
{
	struct drm_castkms_cec_bind_transport bind;
	struct drm_castkms_cec_get_state state;
	int ret;

	ret = cec_bind(fd, connector_id, &bind);
	if (ret) {
		FAIL("get_state_phys_addr", "bind failed: %s", strerror(-ret));
		return;
	}

	ret = cec_get_state(fd, connector_id, bind.transport_id, &state);
	if (ret) {
		FAIL("get_state_phys_addr", "get_state failed: %s",
		     strerror(-ret));
		cec_unbind(fd, connector_id, bind.transport_id);
		return;
	}

	/* EDID was set with PA 1.0.0.0 = 0x1000 */
	if (state.phys_addr != 0x1000) {
		FAIL("get_state_phys_addr",
		     "expected 0x1000, got 0x%04x", state.phys_addr);
	} else {
		PASS("get_state_phys_addr");
	}

	cec_unbind(fd, connector_id, bind.transport_id);
}

static void test_receive_inject(int fd, uint32_t connector_id)
{
	struct drm_castkms_cec_bind_transport bind;
	struct drm_castkms_cec_get_state state;
	int ret;

	ret = cec_bind(fd, connector_id, &bind);
	if (ret) {
		FAIL("receive_inject", "bind failed: %s", strerror(-ret));
		return;
	}

	ret = cec_set_online(fd, connector_id, bind.transport_id, true);
	if (ret) {
		FAIL("receive_inject", "set_online failed: %s", strerror(-ret));
		cec_unbind(fd, connector_id, bind.transport_id);
		return;
	}

	/* Inject a CEC <Give Physical Address> from TV (addr 0) to us */
	uint8_t msg[] = { 0x04, 0x83 }; /* 0->4: Give Physical Address */
	ret = cec_receive(fd, connector_id, bind.transport_id,
			  bind.transport_generation, msg, sizeof(msg));
	if (ret) {
		FAIL("receive_inject", "ioctl failed: %s", strerror(-ret));
		cec_unbind(fd, connector_id, bind.transport_id);
		return;
	}
	PASS("receive_inject");

	ret = cec_get_state(fd, connector_id, bind.transport_id, &state);
	if (ret) {
		FAIL("receive_inject_stats", "get_state failed: %s",
		     strerror(-ret));
		cec_unbind(fd, connector_id, bind.transport_id);
		return;
	}

	if (state.stats_rx != 1) {
		FAIL("receive_inject_stats",
		     "expected 1 rx, got %llu",
		     (unsigned long long)state.stats_rx);
	} else {
		PASS("receive_inject_stats");
	}

	cec_unbind(fd, connector_id, bind.transport_id);
}

static void test_receive_offline_reject(int fd, uint32_t connector_id)
{
	struct drm_castkms_cec_bind_transport bind;
	int ret;

	ret = cec_bind(fd, connector_id, &bind);
	if (ret) {
		FAIL("receive_offline_reject", "bind failed: %s",
		     strerror(-ret));
		return;
	}

	/* Transport is offline by default - receive should fail */
	uint8_t msg[] = { 0x04, 0x83 };
	ret = cec_receive(fd, connector_id, bind.transport_id,
			  bind.transport_generation, msg, sizeof(msg));
	if (ret != -ENONET) {
		FAIL("receive_offline_reject",
		     "expected ENONET, got %s",
		     ret ? strerror(-ret) : "success");
	} else {
		PASS("receive_offline_reject");
	}

	cec_unbind(fd, connector_id, bind.transport_id);
}

static void test_receive_bad_length(int fd, uint32_t connector_id)
{
	struct drm_castkms_cec_bind_transport bind;
	int ret;

	ret = cec_bind(fd, connector_id, &bind);
	if (ret) {
		FAIL("receive_bad_length", "bind failed: %s", strerror(-ret));
		return;
	}

	ret = cec_set_online(fd, connector_id, bind.transport_id, true);
	if (ret) {
		FAIL("receive_bad_length", "set_online failed: %s",
		     strerror(-ret));
		cec_unbind(fd, connector_id, bind.transport_id);
		return;
	}

	/* Length 0 should be rejected */
	uint8_t msg[16] = { 0x04 };
	ret = cec_receive(fd, connector_id, bind.transport_id,
			  bind.transport_generation, msg, 0);
	if (ret != -EINVAL) {
		FAIL("receive_length_zero",
		     "expected EINVAL, got %s",
		     ret ? strerror(-ret) : "success");
	} else {
		PASS("receive_length_zero");
	}

	/* Length 17 should be rejected */
	ret = cec_receive(fd, connector_id, bind.transport_id,
			  bind.transport_generation, msg, 17);
	if (ret != -EINVAL) {
		FAIL("receive_length_too_large",
		     "expected EINVAL, got %s",
		     ret ? strerror(-ret) : "success");
	} else {
		PASS("receive_length_too_large");
	}

	cec_unbind(fd, connector_id, bind.transport_id);
}

static void test_tx_complete_no_pending(int fd, uint32_t connector_id)
{
	struct drm_castkms_cec_bind_transport bind;
	int ret;

	ret = cec_bind(fd, connector_id, &bind);
	if (ret) {
		FAIL("tx_complete_no_pending", "bind failed: %s",
		     strerror(-ret));
		return;
	}

	ret = cec_set_online(fd, connector_id, bind.transport_id, true);
	if (ret) {
		FAIL("tx_complete_no_pending", "set_online failed: %s",
		     strerror(-ret));
		cec_unbind(fd, connector_id, bind.transport_id);
		return;
	}

	/* No TX pending - completing with a bogus cookie should fail */
	ret = cec_tx_complete(fd, connector_id, bind.transport_id,
			      bind.transport_generation, 0xdeadbeef, 1);
	if (ret != -ENOENT) {
		FAIL("tx_complete_no_pending",
		     "expected ENOENT, got %s",
		     ret ? strerror(-ret) : "success");
	} else {
		PASS("tx_complete_no_pending");
	}

	cec_unbind(fd, connector_id, bind.transport_id);
}

static void test_tx_complete_bad_status(int fd, uint32_t connector_id)
{
	struct drm_castkms_cec_bind_transport bind;
	int ret;

	ret = cec_bind(fd, connector_id, &bind);
	if (ret) {
		FAIL("tx_complete_bad_status", "bind failed: %s",
		     strerror(-ret));
		return;
	}

	/* status=0 should be rejected even without a pending TX */
	ret = cec_tx_complete(fd, connector_id, bind.transport_id,
			      bind.transport_generation, 1, 0);
	if (ret != -EINVAL) {
		FAIL("tx_complete_bad_status",
		     "expected EINVAL for status=0, got %s",
		     ret ? strerror(-ret) : "success");
	} else {
		PASS("tx_complete_bad_status");
	}

	cec_unbind(fd, connector_id, bind.transport_id);
}

static void test_state_generation_advances(int fd, uint32_t connector_id)
{
	struct drm_castkms_cec_bind_transport bind;
	struct drm_castkms_cec_get_state state1, state2;
	int ret;

	ret = cec_bind(fd, connector_id, &bind);
	if (ret) {
		FAIL("state_generation_advances", "bind failed: %s",
		     strerror(-ret));
		return;
	}

	ret = cec_get_state(fd, connector_id, bind.transport_id, &state1);
	if (ret) {
		FAIL("state_generation_advances", "get_state failed: %s",
		     strerror(-ret));
		cec_unbind(fd, connector_id, bind.transport_id);
		return;
	}

	ret = cec_set_online(fd, connector_id, bind.transport_id, true);
	if (ret) {
		FAIL("state_generation_advances", "set_online failed: %s",
		     strerror(-ret));
		cec_unbind(fd, connector_id, bind.transport_id);
		return;
	}

	ret = cec_get_state(fd, connector_id, bind.transport_id, &state2);
	if (ret) {
		FAIL("state_generation_advances", "get_state after online failed: %s",
		     strerror(-ret));
		cec_unbind(fd, connector_id, bind.transport_id);
		return;
	}

	if (state2.state_generation <= state1.state_generation) {
		FAIL("state_generation_advances",
		     "expected generation to increase: %llu -> %llu",
		     (unsigned long long)state1.state_generation,
		     (unsigned long long)state2.state_generation);
	} else {
		PASS("state_generation_advances");
	}

	cec_unbind(fd, connector_id, bind.transport_id);
}

static void test_file_close_unbinds(uint32_t connector_id, const char *path)
{
	struct drm_castkms_cec_bind_transport bind, bind2;
	int fd2, ret;

	fd2 = open_device(path);
	if (fd2 < 0) {
		FAIL("file_close_unbinds", "could not open second fd");
		return;
	}

	ret = cec_bind(fd2, connector_id, &bind);
	if (ret) {
		FAIL("file_close_unbinds", "bind failed: %s", strerror(-ret));
		close(fd2);
		return;
	}

	close(fd2);

	/* Transport should now be unbound; binding from a new fd should work */
	fd2 = open_device(path);
	if (fd2 < 0) {
		FAIL("file_close_unbinds", "could not reopen fd");
		return;
	}

	ret = cec_bind(fd2, connector_id, &bind2);
	if (ret) {
		FAIL("file_close_unbinds",
		     "rebind after close failed: %s", strerror(-ret));
	} else {
		PASS("file_close_unbinds");
		cec_unbind(fd2, connector_id, bind2.transport_id);
	}

	close(fd2);
}

static void test_cross_file_bind_reject(uint32_t connector_id, const char *path)
{
	struct drm_castkms_cec_bind_transport bind1, bind2;
	int fd1, fd2, ret;

	fd1 = open_device(path);
	fd2 = open_device(path);
	if (fd1 < 0 || fd2 < 0) {
		FAIL("cross_file_bind_reject", "could not open devices");
		if (fd1 >= 0) close(fd1);
		if (fd2 >= 0) close(fd2);
		return;
	}

	ret = cec_bind(fd1, connector_id, &bind1);
	if (ret) {
		FAIL("cross_file_bind_reject", "first bind failed: %s",
		     strerror(-ret));
		close(fd1);
		close(fd2);
		return;
	}

	ret = cec_bind(fd2, connector_id, &bind2);
	if (ret != -EBUSY) {
		FAIL("cross_file_bind_reject",
		     "expected EBUSY, got %s",
		     ret ? strerror(-ret) : "success");
		if (!ret)
			cec_unbind(fd2, connector_id, bind2.transport_id);
	} else {
		PASS("cross_file_bind_reject");
	}

	cec_unbind(fd1, connector_id, bind1.transport_id);
	close(fd1);
	close(fd2);
}

static void test_stale_generation_reject(int fd, uint32_t connector_id)
{
	struct drm_castkms_cec_bind_transport bind;
	int ret;

	ret = cec_bind(fd, connector_id, &bind);
	if (ret) {
		FAIL("stale_generation_reject", "bind failed: %s",
		     strerror(-ret));
		return;
	}

	ret = cec_set_online(fd, connector_id, bind.transport_id, true);
	if (ret) {
		FAIL("stale_generation_reject", "set_online failed: %s",
		     strerror(-ret));
		cec_unbind(fd, connector_id, bind.transport_id);
		return;
	}

	/* RX with stale generation should fail with ESTALE */
	uint8_t msg[] = { 0x04, 0x83 };
	ret = cec_receive(fd, connector_id, bind.transport_id,
			  bind.transport_generation - 1, msg, sizeof(msg));
	if (ret != -ESTALE) {
		FAIL("stale_generation_reject",
		     "expected ESTALE, got %s",
		     ret ? strerror(-ret) : "success");
	} else {
		PASS("stale_generation_reject");
	}

	cec_unbind(fd, connector_id, bind.transport_id);
}

int main(int argc, char **argv)
{
	const char *path = "/dev/dri/card0";
	uint32_t connector_id;
	int fd;

	if (argc > 1)
		path = argv[1];

	fd = open_device(path);
	if (fd < 0) {
		fprintf(stderr, "Cannot open castkms device at %s\n", path);
		return EXIT_FAILURE;
	}

	if (find_first_display_connector(fd, &connector_id)) {
		fprintf(stderr, "No display connector found\n");
		close(fd);
		return EXIT_FAILURE;
	}

	/* Attach a monitor with CEC-enabled EDID (PA 1.0.0.0) */
	if (attach_monitor_cec(fd, connector_id, 0x10, 0x00)) {
		fprintf(stderr, "Failed to attach monitor: %s\n",
			strerror(errno));
		close(fd);
		return EXIT_FAILURE;
	}

	test_query_caps(fd, connector_id);
	test_query_caps_bad_connector(fd);
	test_query_caps_reserved_reject(fd, connector_id);
	test_bind_unbind(fd, connector_id);
	test_double_bind(fd, connector_id);
	test_unbind_wrong_owner(fd, connector_id);
	test_set_transport_online(fd, connector_id);
	test_get_state_monitor_flag(fd, connector_id);
	test_get_state_stats(fd, connector_id);
	test_get_state_phys_addr(fd, connector_id);
	test_state_generation_advances(fd, connector_id);
	test_receive_inject(fd, connector_id);
	test_receive_offline_reject(fd, connector_id);
	test_receive_bad_length(fd, connector_id);
	test_tx_complete_no_pending(fd, connector_id);
	test_tx_complete_bad_status(fd, connector_id);
	test_stale_generation_reject(fd, connector_id);
	test_file_close_unbinds(connector_id, path);
	test_cross_file_bind_reject(connector_id, path);

	detach_monitor(fd, connector_id);

	printf("\n%d/%d tests passed\n", tests_pass, tests_run);
	close(fd);

	return tests_pass == tests_run ? EXIT_SUCCESS : EXIT_FAILURE;
}
