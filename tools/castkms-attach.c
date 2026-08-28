// SPDX-License-Identifier: GPL-2.0-only

#include <drm/castkms_drm.h>
#include <drm/drm_mode.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "castkms-test-drm.h"
#include "virtualscreen-edid.h"

#ifndef DRM_MODE_CONNECTED
#define DRM_MODE_CONNECTED 1
#endif

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
	    (grant.rights & (DRM_CASTKMS_GRANT_MANAGE_ATTACHMENT |
			     DRM_CASTKMS_GRANT_UPDATE_EDID)) !=
		    (DRM_CASTKMS_GRANT_MANAGE_ATTACHMENT |
		     DRM_CASTKMS_GRANT_UPDATE_EDID) ||
	    grant.rights & ~DRM_CASTKMS_GRANT_RIGHTS_MASK ||
	    grant.flags & ~DRM_CASTKMS_GRANT_FLAGS_MASK ||
	    grant.state == DRM_CASTKMS_GRANT_STATE_REVOKED ||
	    grant.state > DRM_CASTKMS_GRANT_STATE_REVOKED ||
	    grant.reserved) {
		fprintf(stderr, "inherited fd is not a usable attachment grant\n");
		close(fd);
		return -1;
	}

	*connector_id = grant.connector_id;
	return fd;
}

static int attach_monitor(int fd, uint32_t connector_id,
			  const void *edid, uint32_t edid_size)
{
	struct drm_castkms_capture_attach_monitor attach = {
		.connector_id = connector_id,
		.edid_size = edid_size,
		.edid_ptr = (uint64_t)(uintptr_t)edid,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_ATTACH_MONITOR, &attach) < 0)
		return -errno;
	return 0;
}

static int detach_monitor(int fd, uint32_t connector_id)
{
	struct drm_castkms_capture_detach_monitor detach = {
		.connector_id = connector_id,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_DETACH_MONITOR, &detach) < 0)
		return -errno;
	return 0;
}

static int connector_is_connected(int fd, uint32_t connector_id)
{
	struct drm_mode_get_connector connector = {
		.connector_id = connector_id,
	};

	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) < 0)
		return -errno;
	return connector.connection == DRM_MODE_CONNECTED ? 0 : -ENOTCONN;
}

static void usage(const char *program)
{
	fprintf(stderr, "usage: %s [--grant-fd FD]\n", program);
}

int main(int argc, char **argv)
{
	uint8_t edid[CASTKMS_REFERENCE_EDID_MAX_SIZE];
	uint32_t connector_id;
	char discard;
	ssize_t read_ret;
	int inherited_fd = -1;
	int edid_size;
	int ioctl_ret;
	int fd;
	int ret = EXIT_FAILURE;

	if (argc == 2 && (!strcmp(argv[1], "-h") ||
			  !strcmp(argv[1], "--help"))) {
		usage(argv[0]);
		return EXIT_SUCCESS;
	}
	if (argc == 3 && !strcmp(argv[1], "--grant-fd")) {
		if (parse_fd(argv[2], &inherited_fd)) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	} else if (argc != 1) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	fd = open_grant(inherited_fd, &connector_id);
	if (fd < 0)
		return EXIT_FAILURE;

	edid_size = castkms_fill_edid(edid, sizeof(edid), NULL,
				      CASTKMS_EDID_FLAG_AUDIO);
	if (edid_size < 0) {
		fprintf(stderr, "failed to build attachment EDID\n");
		goto out_close;
	}
	ioctl_ret = attach_monitor(fd, connector_id, edid,
				   (uint32_t)edid_size);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("ATTACH_MONITOR");
		goto out_close;
	}
	if (connector_is_connected(fd, connector_id)) {
		fprintf(stderr, "attached connector is not connected\n");
		goto out_detach;
	}

	printf("connector_id=%u\n", connector_id);
	printf("attached=1\n");
	fflush(stdout);
	do {
		read_ret = read(STDIN_FILENO, &discard, 1);
	} while (read_ret < 0 && errno == EINTR);
	if (read_ret < 0) {
		perror("attachment release gate");
		goto out_detach;
	}
	ret = EXIT_SUCCESS;

out_detach:
	if (detach_monitor(fd, connector_id) && ret == EXIT_SUCCESS) {
		perror("DETACH_MONITOR");
		ret = EXIT_FAILURE;
	}
out_close:
	close(fd);
	return ret;
}
