// SPDX-License-Identifier: GPL-2.0-only

#include <drm/castkms_drm.h>
#include <drm/drm_fourcc.h>
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

static_assert(sizeof(struct drm_castkms_capture_format) == 16,
	      "capture format ABI size changed");
static_assert(sizeof(struct drm_castkms_capture_query_caps) == 40,
	      "capture query ABI size changed");
static_assert(sizeof(struct drm_castkms_capture_start) == 24,
	      "capture start ABI size changed");
static_assert(sizeof(struct drm_castkms_capture_stop) == 16,
	      "capture stop ABI size changed");
static_assert(sizeof(struct drm_castkms_get_grant) == 32,
	      "get-grant ABI size changed");
static_assert(sizeof(struct drm_event_castkms_grant_revoked) == 24,
	      "grant-revoked event ABI size changed");
static_assert(sizeof(struct drm_event_castkms_grant_state) == 32,
	      "grant-state event ABI size changed");

static int ensure_non_master(int fd, bool report)
{
	if (ioctl(fd, DRM_IOCTL_DROP_MASTER, 0) < 0 && errno != EINVAL) {
		perror("DRM_IOCTL_DROP_MASTER");
		return -1;
	}

	if (report)
		printf("capture_non_master=1\n");
	return 0;
}

static int open_capture_device(const char *path, bool report_non_master)
{
	int fd;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open DRM device");
		return -1;
	}

	if (castkms_test_check_driver_name(fd) ||
	    ensure_non_master(fd, report_non_master)) {
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

static int open_capture_grant(int inherited_fd, uint32_t required_rights,
			      bool report)
{
	struct drm_castkms_get_grant grant = {};
	const char *environment;
	int fd;

	if (inherited_fd < 0) {
		environment = getenv("CASTKMS_GRANT_FD");
		if (!environment || parse_fd(environment, &inherited_fd)) {
			fprintf(stderr,
				"an inherited CastKMS grant fd is required\n");
			return -1;
		}
	}

	fd = fcntl(inherited_fd, F_DUPFD_CLOEXEC, 3);
	if (fd < 0) {
		perror("duplicate grant fd");
		return -1;
	}
	if (castkms_test_check_driver_name(fd))
		goto fail;
	if (!(fcntl(fd, F_GETFL) & O_NONBLOCK)) {
		fprintf(stderr, "grant fd must be nonblocking\n");
		goto fail;
	}
	if (ioctl(fd, DRM_IOCTL_CASTKMS_GET_GRANT, &grant) < 0) {
		perror("DRM_IOCTL_CASTKMS_GET_GRANT");
		goto fail;
	}
	if (!grant.grant_id || !grant.connector_id ||
	    (grant.rights & required_rights) != required_rights ||
	    grant.rights & ~DRM_CASTKMS_GRANT_RIGHTS_MASK ||
	    grant.flags & ~DRM_CASTKMS_GRANT_FLAGS_MASK ||
	    grant.state == DRM_CASTKMS_GRANT_STATE_REVOKED ||
	    grant.state > DRM_CASTKMS_GRANT_STATE_REVOKED ||
	    grant.reserved || grant.reserved2) {
		fprintf(stderr, "inherited fd is not a usable capture grant\n");
		goto fail;
	}
	if (report) {
		printf("capture_grant_fd=1\n");
		printf("capture_grant_id=%u\n", grant.grant_id);
	}
	return fd;

fail:
	close(fd);
	return -1;
}

static int start_capture(int fd, uint32_t crtc_id,
			 struct drm_castkms_capture_start *start)
{
	return castkms_test_capture_start(
		fd, crtc_id, DRM_CASTKMS_CAPTURE_START_EXCLUSIVE, start);
}

static int stop_capture(int fd, uint32_t stream_id)
{
	return castkms_test_capture_stop(fd, stream_id);
}

static int connector_drives_crtc(int fd, uint32_t connector_id,
				 uint32_t crtc_id, bool *drives)
{
	uint32_t crtc_ids[8];
	uint32_t encoder_ids[8];
	struct drm_mode_card_res res = {
		.count_crtcs = 8,
		.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids,
	};
	struct drm_mode_get_connector conn = {
		.connector_id = connector_id,
	};
	int crtc_idx = -1;
	uint32_t i;

	*drives = false;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
		return -1;
	if (!res.count_crtcs || res.count_crtcs > 8)
		return -1;
	for (i = 0; i < res.count_crtcs; i++) {
		if (crtc_ids[i] == crtc_id) {
			crtc_idx = (int)i;
			break;
		}
	}
	if (crtc_idx < 0)
		return -1;
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
		return -1;
	if (conn.connector_type == DRM_MODE_CONNECTOR_WRITEBACK)
		return 0;
	if (!conn.count_encoders || conn.count_encoders > 8)
		return -1;
	conn.count_modes = 0;
	conn.count_props = 0;
	conn.encoders_ptr = (uint64_t)(uintptr_t)encoder_ids;
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
		return -1;
	if (conn.count_encoders > 8)
		return -1;
	for (i = 0; i < conn.count_encoders; i++) {
		struct drm_mode_get_encoder enc = {
			.encoder_id = encoder_ids[i],
		};

		if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) < 0)
			return -1;
		if (enc.possible_crtcs & (1u << crtc_idx)) {
			*drives = true;
			return 0;
		}
	}

	return 0;
}

static int find_display_connector(int fd, uint32_t crtc_id,
				  uint32_t *connector_id)
{
	uint32_t connector_ids[32];
	struct drm_mode_card_res res = {
		.count_connectors = 32,
		.connector_id_ptr = (uint64_t)(uintptr_t)connector_ids,
	};
	uint32_t i;

	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
		return -1;
	if (!res.count_connectors || res.count_connectors > 32)
		return -1;
	for (i = 0; i < res.count_connectors; i++) {
		bool drives = false;

		if (connector_drives_crtc(fd, connector_ids[i], crtc_id,
					  &drives))
			return -1;
		if (drives) {
			*connector_id = connector_ids[i];
			return 0;
		}
	}

	return -1;
}

static int parse_crtc_id(const char *text, uint32_t *crtc_id)
{
	char *end;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 0);
	if (errno || *text == '\0' || *end != '\0' || value > UINT32_MAX) {
		fprintf(stderr, "invalid CRTC ID: %s\n", text);
		return -1;
	}

	*crtc_id = value;
	return 0;
}

static int validate_query(const struct drm_castkms_capture_query_caps *query,
			  uint32_t crtc_id)
{
	if (query->uapi_major != DRM_CASTKMS_CAPTURE_UAPI_MAJOR ||
	    query->uapi_minor < DRM_CASTKMS_CAPTURE_UAPI_MINOR) {
		fprintf(stderr, "unsupported capture UAPI version: %u.%u\n",
			query->uapi_major, query->uapi_minor);
		return -1;
	}
	if (query->crtc_id != crtc_id || query->format_count != 1 ||
	    query->reserved || query->max_registered_buffers) {
		fprintf(stderr, "unexpected capture query result\n");
		return -1;
	}
	if (query->flags & DRM_CASTKMS_CAPTURE_CAP_IMPLICIT_SYNC) {
		fprintf(stderr,
			"capture query unexpectedly advertises implicit sync\n");
		return -1;
	}
	if (!(query->flags & DRM_CASTKMS_CAPTURE_CAP_GRANT_FD)) {
		fprintf(stderr, "capture query lacks grant-fd support\n");
		return -1;
	}

	return 0;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s [--grant-fd FD] [--mode-generation] DRM-DEVICE CRTC-ID\n",
		program);
}

int main(int argc, char **argv)
{
	struct drm_castkms_capture_format format = {};
	struct drm_castkms_capture_query_caps query = {};
	struct drm_castkms_capture_start first_stream;
	struct drm_castkms_capture_start second_stream;
	uint32_t crtc_id;
	int competitor_fd = -1;
	int fd;
	int ioctl_ret;
	int ret = EXIT_FAILURE;
	const char *device;
	const char *mode = NULL;
	int inherited_fd = -1;
	int argument = 1;

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
	if (argument < argc &&
	    !strcmp(argv[argument], "--mode-generation"))
		mode = argv[argument++];
	if (argc - argument != 2) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	device = argv[argument];
	if (parse_crtc_id(argv[argument + 1], &crtc_id))
		return EXIT_FAILURE;

	if (mode) {
		struct drm_castkms_capture_start stream;

		fd = open_capture_grant(inherited_fd,
			DRM_CASTKMS_GRANT_CAPTURE_PIXELS |
			DRM_CASTKMS_GRANT_READ_CURSOR, false);
		if (fd < 0)
			return EXIT_FAILURE;
		ioctl_ret = start_capture(fd, crtc_id, &stream);
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("start capture stream");
			close(fd);
			return EXIT_FAILURE;
		}
		printf("capture_mode_generation=%llu\n",
		       (unsigned long long)stream.mode_generation);
		ioctl_ret = stop_capture(fd, stream.stream_id);
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("stop capture stream");
			close(fd);
			return EXIT_FAILURE;
		}
		close(fd);
		return EXIT_SUCCESS;
	}

	fd = open_capture_grant(inherited_fd,
		DRM_CASTKMS_GRANT_CAPTURE_PIXELS |
		DRM_CASTKMS_GRANT_READ_CURSOR, true);
	if (fd < 0)
		return EXIT_FAILURE;

	query.crtc_id = crtc_id;
	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_QUERY_CAPS, &query) < 0) {
		perror("capture capability count query");
		goto out_close;
	}
	if (validate_query(&query, crtc_id))
		goto out_close;

	query = (struct drm_castkms_capture_query_caps) {
		.crtc_id = crtc_id,
		.format_count = 1,
		.formats_ptr = (uint64_t)(uintptr_t)&format,
	};
	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_QUERY_CAPS, &query) < 0) {
		perror("capture capability format query");
		goto out_close;
	}
	if (validate_query(&query, crtc_id))
		goto out_close;
	if (format.format != DRM_FORMAT_XRGB8888 || format.flags ||
	    format.modifier != DRM_FORMAT_MOD_LINEAR) {
		fprintf(stderr,
			"unexpected capture format: %#x flags=%#x modifier=%#llx\n",
			format.format, format.flags,
			(unsigned long long)format.modifier);
		goto out_close;
	}

	printf("capture_uapi=%u.%u\n", query.uapi_major, query.uapi_minor);
	printf("capture_format=XRGB8888:LINEAR\n");
	printf("capture_max_registered_buffers=%u\n",
	       query.max_registered_buffers);
	printf("capture_dmabuf_import=%s\n",
	       query.flags & DRM_CASTKMS_CAPTURE_CAP_DMA_BUF_IMPORT ?
	       "supported" : "unsupported");
	printf("capture_query=pass\n");

	competitor_fd = open_capture_device(device, false);
	if (competitor_fd < 0)
		goto out_close;
	ioctl_ret = start_capture(competitor_fd, crtc_id, &second_stream);
	if (ioctl_ret != -EACCES) {
		fprintf(stderr,
			"ordinary-fd capture start returned %d, expected %d\n",
			ioctl_ret, -EACCES);
		goto out_close;
	}
	printf("capture_plain_fd_denied=pass\n");

	{
		uint32_t connector_id;

		if (find_display_connector(fd, crtc_id, &connector_id)) {
			fprintf(stderr, "failed to find display connector\n");
			goto out_close;
		}
		ioctl_ret = start_capture(fd, crtc_id, &first_stream);
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("start capture stream");
			goto out_close;
		}
		if (!first_stream.stream_id || !first_stream.mode_generation) {
			fprintf(stderr,
				"capture start returned invalid stream metadata\n");
			goto out_close;
		}
		ioctl_ret = stop_capture(competitor_fd, first_stream.stream_id);
		if (ioctl_ret != -ENOENT) {
			fprintf(stderr,
				"ordinary-fd capture stop returned %d, expected %d\n",
				ioctl_ret, -ENOENT);
			goto out_close;
		}
	}

	ioctl_ret = start_capture(fd, crtc_id, &second_stream);
	if (ioctl_ret != -EBUSY) {
		fprintf(stderr,
			"second capture start returned %d, expected %d\n",
			ioctl_ret, -EBUSY);
		goto out_close;
	}
	printf("capture_stream_exclusive=pass\n");

	ioctl_ret = stop_capture(fd, first_stream.stream_id);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("stop first capture stream");
		goto out_close;
	}
	ioctl_ret = start_capture(fd, crtc_id, &second_stream);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("start capture stream after stop");
		goto out_close;
	}
	if (second_stream.mode_generation != first_stream.mode_generation) {
		fprintf(stderr, "mode generation changed without a modeset\n");
		goto out_close;
	}
	printf("capture_stream_stop=pass\n");
	ioctl_ret = stop_capture(fd, second_stream.stream_id);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("stop restarted capture stream");
		goto out_close;
	}

	printf("capture_mode_generation=%llu\n",
	       (unsigned long long)second_stream.mode_generation);
	printf("capture_stream_lifecycle=pass\n");
	ret = EXIT_SUCCESS;

out_close:
	if (competitor_fd >= 0)
		close(competitor_fd);
	close(fd);
	return ret;
}
