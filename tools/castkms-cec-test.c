// SPDX-License-Identifier: GPL-2.0-only

#include <drm/castkms_drm.h>
#include <drm/drm_mode.h>

#include <linux/cec.h>

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

#include "castkms-test-drm.h"
#include "virtualscreen-edid.h"

/* ABI size checks */
static_assert(sizeof(struct drm_castkms_cec_query_caps) == 40,
	      "cec query caps ABI size changed");
static_assert(sizeof(struct drm_castkms_cec_bind_transport) == 48,
	      "cec bind transport ABI size changed");
static_assert(offsetof(struct drm_castkms_cec_bind_transport, pad0) == 44,
	      "cec bind transport ABI layout changed");
static_assert(sizeof(struct drm_castkms_cec_unbind_transport) == 16,
	      "cec unbind transport ABI size changed");
static_assert(sizeof(struct drm_castkms_cec_set_transport_state) == 16,
	      "cec set transport state ABI size changed");
static_assert(sizeof(struct drm_castkms_cec_tx_complete) == 32,
	      "cec tx complete ABI size changed");
static_assert(sizeof(struct drm_castkms_cec_receive) == 40,
	      "cec receive ABI size changed");
static_assert(offsetof(struct drm_castkms_cec_receive, pad0) == 35,
	      "cec receive ABI layout changed");
static_assert(sizeof(struct drm_castkms_cec_get_state) == 112,
	      "cec get state ABI size changed");
static_assert(offsetof(struct drm_castkms_cec_get_state, pending_cookie) == 48,
	      "cec get state ABI layout changed");
static_assert(sizeof(struct drm_castkms_cec_event_tx) == 72,
	      "cec event tx ABI size changed");
static_assert(sizeof(struct drm_castkms_get_grant) == 32,
	      "get grant ABI size changed");

static int tests_run;
static int tests_pass;

#define PASS(name) do { tests_run++; tests_pass++; \
	printf("%-55s pass\n", name); } while (0)
#define FAIL(name, ...) do { tests_run++; \
	printf("%-55s FAIL: ", name); printf(__VA_ARGS__); printf("\n"); } while (0)

static int open_device(const char *path)
{
	int fd;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open DRM device");
		return -1;
	}

	if (castkms_test_check_driver_name(fd)) {
		close(fd);
		return -1;
	}

	if (ioctl(fd, DRM_IOCTL_DROP_MASTER, 0) < 0 && errno != EINVAL) {
		close(fd);
		return -1;
	}

	return fd;
}

static int parse_fd(const char *text, int *fd)
{
	char *end = NULL;
	long value;

	errno = 0;
	value = strtol(text, &end, 10);
	if (errno || !*text || !end || *end || value < 0 || value > INT32_MAX)
		return -1;
	*fd = (int)value;
	return 0;
}

static int open_grant(int inherited_fd, uint32_t *connector_id)
{
	const uint32_t rights = DRM_CASTKMS_GRANT_MANAGE_ATTACHMENT |
		DRM_CASTKMS_GRANT_MANAGE_CEC;
	struct drm_castkms_get_grant grant = {};
	const char *environment;
	int fd;

	if (inherited_fd < 0) {
		environment = getenv("CASTKMS_GRANT_FD");
		if (!environment || parse_fd(environment, &inherited_fd)) {
			fprintf(stderr, "an inherited CastKMS grant fd is required\n");
			return -1;
		}
	}

	fd = fcntl(inherited_fd, F_DUPFD_CLOEXEC, 3);
	if (fd < 0) {
		perror("duplicate grant fd");
		return -1;
	}
	if (castkms_test_check_driver_name(fd) ||
	    ioctl(fd, DRM_IOCTL_CASTKMS_GET_GRANT, &grant) < 0) {
		perror("DRM_IOCTL_CASTKMS_GET_GRANT");
		close(fd);
		return -1;
	}
	if (!grant.grant_id || !grant.connector_id ||
	    (grant.rights & rights) != rights ||
	    grant.state == DRM_CASTKMS_GRANT_STATE_REVOKED ||
	    grant.state > DRM_CASTKMS_GRANT_STATE_REVOKED ||
	    grant.reserved) {
		fprintf(stderr, "inherited fd is not a usable CEC grant\n");
		close(fd);
		return -1;
	}
	*connector_id = grant.connector_id;
	return fd;
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

static int open_cec_adapter(uint32_t connector_id)
{
	for (unsigned int index = 0; index < 64; index++) {
		struct cec_connector_info connector = {};
		struct cec_caps caps = {};
		char path[32];
		bool matches = false;
		int fd;

		snprintf(path, sizeof(path), "/dev/cec%u", index);
		fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
		if (fd < 0)
			continue;
		if (ioctl(fd, CEC_ADAP_G_CAPS, &caps) < 0 ||
		    !(caps.capabilities & CEC_CAP_TRANSMIT)) {
			close(fd);
			continue;
		}
		if (caps.capabilities & CEC_CAP_CONNECTOR_INFO) {
			if (!ioctl(fd, CEC_ADAP_G_CONNECTOR_INFO, &connector) &&
			    connector.type == CEC_CONNECTOR_TYPE_DRM &&
			    connector.drm.connector_id == connector_id)
				matches = true;
		} else if (!strncmp(caps.driver, "castkms",
				   sizeof(caps.driver))) {
			matches = true;
		}
		if (matches)
			return fd;
		close(fd);
	}

	return -1;
}

static int read_cec_tx_event(int fd, struct drm_castkms_cec_event_tx *event)
{
	struct pollfd poll_fd = {
		.fd = fd,
		.events = POLLIN,
	};
	uint8_t buffer[4096];

	for (unsigned int attempt = 0; attempt < 16; attempt++) {
		ssize_t size;
		int ret;

		do {
			ret = poll(&poll_fd, 1, 3000);
		} while (ret < 0 && errno == EINTR);
		if (ret <= 0)
			return ret ? -errno : -ETIMEDOUT;

		size = read(fd, buffer, sizeof(buffer));
		if (size < 0)
			return -errno;
		for (size_t offset = 0; offset < (size_t)size;) {
			struct drm_event base;

			if ((size_t)size - offset < sizeof(base))
				return -EPROTO;
			memcpy(&base, buffer + offset, sizeof(base));
			if (base.length < sizeof(base) ||
			    base.length > (size_t)size - offset)
				return -EPROTO;
			if (base.type == DRM_CASTKMS_CEC_EVENT_TX) {
				if (base.length != sizeof(*event))
					return -EPROTO;
				memcpy(event, buffer + offset, sizeof(*event));
				return 0;
			}
			offset += base.length;
		}
	}
	return -ETIMEDOUT;
}

static int read_cec_tx_result(int fd, struct cec_msg *message)
{
	struct pollfd poll_fd = {
		.fd = fd,
		.events = POLLIN,
	};
	int ret;

	do {
		ret = poll(&poll_fd, 1, 3000);
	} while (ret < 0 && errno == EINTR);
	if (ret <= 0)
		return ret ? -errno : -ETIMEDOUT;

	*message = (struct cec_msg) {};
	if (ioctl(fd, CEC_RECEIVE, message) < 0)
		return -errno;
	return 0;
}

static int configure_cec_adapter(int fd)
{
	struct cec_log_addrs addresses = {
		.cec_version = CEC_OP_CEC_VERSION_2_0,
		.num_log_addrs = 1,
		.vendor_id = CEC_VENDOR_ID_NONE,
	};
	struct cec_caps caps = {};
	int flags;
	int ret = 0;

	if (ioctl(fd, CEC_ADAP_G_CAPS, &caps) < 0)
		return -errno;
	if (!(caps.capabilities & CEC_CAP_LOG_ADDRS))
		return 0;

	addresses.primary_device_type[0] = CEC_OP_PRIM_DEVTYPE_SWITCH;
	addresses.log_addr_type[0] = CEC_LOG_ADDR_TYPE_UNREGISTERED;
	addresses.all_device_types[0] = CEC_OP_ALL_DEVTYPE_SWITCH;
	memcpy(addresses.osd_name, "CastKMS test", sizeof("CastKMS test"));

	/*
	 * The adapter fd is nonblocking so CEC_TRANSMIT returns a sequence for
	 * later completion.  Logical-address setup has different semantics in
	 * that mode: the ioctl returns before the core has installed the address.
	 * Configure this unregistered switch synchronously, then restore the fd.
	 */
	flags = fcntl(fd, F_GETFL);
	if (flags < 0)
		return -errno;
	if (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0)
		return -errno;
	if (ioctl(fd, CEC_ADAP_S_LOG_ADDRS, &addresses) < 0)
		ret = -errno;
	if (fcntl(fd, F_SETFL, flags) < 0 && !ret)
		ret = -errno;
	if (ret)
		return ret;
	if (addresses.num_log_addrs != 1 ||
	    addresses.log_addr[0] != CEC_LOG_ADDR_UNREGISTERED)
		return -EPROTO;
	return 0;
}

static int unconfigure_cec_adapter(int fd)
{
	struct cec_log_addrs addresses = {};

	if (ioctl(fd, CEC_ADAP_S_LOG_ADDRS, &addresses) < 0)
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
		cec_unbind(fd, connector_id, bind.transport_id);
		return;
	}
	PASS("bind_transport_id");

	if (!bind.transport_generation) {
		FAIL("bind_transport_generation", "got zero generation");
		cec_unbind(fd, connector_id, bind.transport_id);
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

static void test_real_cec_transmit(int fd, uint32_t connector_id)
{
	struct drm_castkms_cec_event_tx event = {};
	struct drm_castkms_cec_bind_transport bind;
	struct cec_msg message;
	struct cec_msg result;
	uint32_t mode = CEC_MODE_INITIATOR;
	bool adapter_configured = false;
	bool request_found = false;
	int cec_fd = -1;
	int ret;

	ret = cec_bind(fd, connector_id, &bind);
	if (ret) {
		FAIL("real_cec_transmit", "bind failed: %s", strerror(-ret));
		return;
	}
	ret = cec_set_online(fd, connector_id, bind.transport_id, true);
	if (ret) {
		FAIL("real_cec_transmit", "set_online failed: %s",
		     strerror(-ret));
		goto out_unbind;
	}

	cec_fd = open_cec_adapter(connector_id);
	if (cec_fd < 0) {
		FAIL("real_cec_transmit", "matching /dev/cec adapter not found");
		goto out_unbind;
	}
	if (ioctl(cec_fd, CEC_S_MODE, &mode) < 0) {
		FAIL("real_cec_transmit", "CEC_S_MODE failed: %s",
		     strerror(errno));
		goto out_close;
	}
	ret = configure_cec_adapter(cec_fd);
	if (ret) {
		FAIL("real_cec_transmit", "CEC adapter setup failed: %s",
		     strerror(-ret));
		goto out_close;
	}
	adapter_configured = true;

	cec_msg_init(&message, CEC_LOG_ADDR_UNREGISTERED,
		     CEC_LOG_ADDR_BROADCAST);
	message.msg[message.len++] = CEC_MSG_REQUEST_ACTIVE_SOURCE;
	if (ioctl(cec_fd, CEC_TRANSMIT, &message) < 0) {
		FAIL("real_cec_transmit", "CEC_TRANSMIT failed: %s",
		     strerror(errno));
		goto out_close;
	}
	if (!message.sequence) {
		FAIL("real_cec_transmit", "CEC_TRANSMIT returned no sequence");
		goto out_close;
	}

	for (unsigned int attempt = 0; attempt < 8; attempt++) {
		ret = read_cec_tx_event(fd, &event);
		if (ret) {
			FAIL("real_cec_transmit", "TX event failed: %s",
			     strerror(-ret));
			goto out_close;
		}
		if (event.transport_id != bind.transport_id ||
		    event.transport_generation != bind.transport_generation ||
		    event.connector_id != connector_id || !event.cookie) {
			FAIL("real_cec_transmit", "TX event identity mismatch");
			goto out_close;
		}
		if (event.length == 2 && event.msg[0] == 0xff &&
		    event.msg[1] == CEC_MSG_REQUEST_ACTIVE_SOURCE) {
			request_found = true;
			break;
		}

		/* Complete CEC-core setup traffic before the queued test message. */
		ret = cec_tx_complete(fd, connector_id, bind.transport_id,
				      event.transport_generation, event.cookie,
				      CEC_TX_STATUS_OK);
		if (ret) {
			FAIL("real_cec_transmit", "setup TX completion failed: %s",
			     strerror(-ret));
			goto out_close;
		}
	}
	if (!request_found) {
		FAIL("real_cec_transmit", "test TX event was not delivered");
		goto out_close;
	}

	ret = cec_tx_complete(fd, connector_id, bind.transport_id,
			      event.transport_generation, event.cookie,
			      CEC_TX_STATUS_OK);
	if (ret) {
		FAIL("real_cec_transmit", "TX completion failed: %s",
		     strerror(-ret));
		goto out_close;
	}
	ret = read_cec_tx_result(cec_fd, &result);
	if (ret) {
		FAIL("real_cec_transmit", "CEC result failed: %s", strerror(-ret));
		goto out_close;
	}
	if (result.sequence != message.sequence ||
	    !(result.tx_status & CEC_TX_STATUS_OK)) {
		FAIL("real_cec_transmit", "CEC completion payload mismatch");
		goto out_close;
	}
	ret = unconfigure_cec_adapter(cec_fd);
	if (ret) {
		FAIL("real_cec_transmit", "CEC adapter cleanup failed: %s",
		     strerror(-ret));
		goto out_close;
	}
	adapter_configured = false;

	PASS("real_cec_transmit");

out_close:
	if (adapter_configured)
		(void)unconfigure_cec_adapter(cec_fd);
	close(cec_fd);
out_unbind:
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

static void test_plain_fd_denied(uint32_t connector_id, const char *path)
{
	struct drm_castkms_cec_bind_transport bind;
	int fd;
	int ret;

	fd = open_device(path);
	if (fd < 0) {
		FAIL("plain_fd_denied", "could not open ordinary fd");
		return;
	}

	ret = cec_bind(fd, connector_id, &bind);
	if (ret != -EACCES) {
		FAIL("plain_fd_denied", "expected EACCES, got %s",
		     ret ? strerror(-ret) : "success");
		if (!ret)
			cec_unbind(fd, connector_id, bind.transport_id);
	} else {
		PASS("plain_fd_denied");
	}
	close(fd);
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

static void usage(const char *program)
{
	fprintf(stderr, "usage: %s [--grant-fd FD] [DRM-DEVICE]\n", program);
}

int main(int argc, char **argv)
{
	const char *path = "/dev/dri/card0";
	uint32_t connector_id;
	int inherited_fd = -1;
	int argument = 1;
	int fd;

	if (argc == 2 && (!strcmp(argv[1], "-h") ||
			  !strcmp(argv[1], "--help"))) {
		usage(argv[0]);
		return EXIT_SUCCESS;
	}
	if (argc - argument >= 2 && !strcmp(argv[argument], "--grant-fd")) {
		if (parse_fd(argv[argument + 1], &inherited_fd)) {
			fprintf(stderr, "invalid grant fd: %s\n",
				argv[argument + 1]);
			return EXIT_FAILURE;
		}
		argument += 2;
	}
	if (argc - argument > 1) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (argument < argc)
		path = argv[argument];

	fd = open_grant(inherited_fd, &connector_id);
	if (fd < 0) {
		fprintf(stderr, "Cannot consume CastKMS CEC grant\n");
		return EXIT_FAILURE;
	}

	/* Attach a monitor with CEC-enabled EDID (PA 1.0.0.0) */
	{
		int attach_ret = attach_monitor_cec(fd, connector_id, 0x10, 0x00);

		if (attach_ret) {
			fprintf(stderr, "Failed to attach monitor: %s\n",
				strerror(-attach_ret));
			close(fd);
			return EXIT_FAILURE;
		}
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
	test_real_cec_transmit(fd, connector_id);
	test_tx_complete_no_pending(fd, connector_id);
	test_tx_complete_bad_status(fd, connector_id);
	test_stale_generation_reject(fd, connector_id);
	test_plain_fd_denied(connector_id, path);

	detach_monitor(fd, connector_id);

	printf("\n%d/%d tests passed\n", tests_pass, tests_run);
	close(fd);

	return tests_pass == tests_run ? EXIT_SUCCESS : EXIT_FAILURE;
}
