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
	    grant.reserved || grant.reserved2) {
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

	PASS("set_online");

	ret = cec_set_online(fd, connector_id, bind.transport_id, false);
	if (ret) {
		FAIL("set_offline", "ioctl failed: %s", strerror(-ret));
		cec_unbind(fd, connector_id, bind.transport_id);
		return;
	}

	PASS("set_offline");

	cec_unbind(fd, connector_id, bind.transport_id);
}


static void test_real_cec_transmit(int fd, uint32_t connector_id)
{
	struct drm_castkms_cec_event_tx event = {};
	struct drm_castkms_cec_bind_transport bind;
	struct cec_msg message;
	uint32_t mode = CEC_MODE_INITIATOR;
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
	if (event.length != message.len ||
	    memcmp(event.msg, message.msg, message.len)) {
		FAIL("real_cec_transmit", "TX event payload mismatch");
		goto out_close;
	}

	PASS("real_cec_transmit");

out_close:
	close(cec_fd);
out_unbind:
	cec_unbind(fd, connector_id, bind.transport_id);
}

static void usage(const char *program)
{
	fprintf(stderr, "usage: %s [--grant-fd FD]\n", program);
}

int main(int argc, char **argv)
{
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
	if (argc - argument > 0) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

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
	test_real_cec_transmit(fd, connector_id);
	detach_monitor(fd, connector_id);

	printf("\n%d/%d tests passed\n", tests_pass, tests_run);
	close(fd);

	return tests_pass == tests_run ? EXIT_SUCCESS : EXIT_FAILURE;
}
