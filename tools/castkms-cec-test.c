// SPDX-License-Identifier: GPL-2.0-only

#include <drm/castkms_drm.h>
#include <drm/drm_mode.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
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

	uint64_t expected_caps = DRM_CASTKMS_CEC_CAP_EDID_PHYS_ADDR;
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
		return;
	}
	PASS("double_bind_reject");

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
	test_double_bind(fd, connector_id);
	detach_monitor(fd, connector_id);

	printf("\n%d/%d tests passed\n", tests_pass, tests_run);
	close(fd);

	return tests_pass == tests_run ? EXIT_SUCCESS : EXIT_FAILURE;
}
