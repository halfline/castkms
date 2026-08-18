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
#include <sys/mman.h>
#include <unistd.h>

static_assert(sizeof(struct drm_castkms_capture_format) == 16,
	      "capture format ABI size changed");
static_assert(sizeof(struct drm_castkms_capture_query_caps) == 40,
	      "capture query ABI size changed");
static_assert(sizeof(struct drm_castkms_capture_start) == 24,
	      "capture start ABI size changed");

static int check_driver_name(int fd)
{
	struct drm_version version = {};
	char name[32] = {};

	version.name = name;
	version.name_len = sizeof(name) - 1;
	if (ioctl(fd, DRM_IOCTL_VERSION, &version) < 0) {
		perror("DRM_IOCTL_VERSION");
		return -1;
	}

	if (version.name_len != strlen("castkms") ||
	    memcmp(name, "castkms", strlen("castkms"))) {
		fprintf(stderr, "unexpected DRM driver: %.*s\n",
			(int)version.name_len, name);
		return -1;
	}

	return 0;
}

static int require_capability(int fd, uint64_t capability, const char *name)
{
	struct drm_get_cap cap = {
		.capability = capability,
	};

	if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) < 0) {
		perror(name);
		return -1;
	}
	if (cap.value != 1) {
		fprintf(stderr, "%s is not enabled\n", name);
		return -1;
	}

	printf("%s=1\n", name);
	return 0;
}

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

	if (check_driver_name(fd) || ensure_non_master(fd, report_non_master)) {
		close(fd);
		return -1;
	}

	return fd;
}

static int start_capture(int fd, uint32_t crtc_id,
			 struct drm_castkms_capture_start *start)
{
	*start = (struct drm_castkms_capture_start) {
		.crtc_id = crtc_id,
		.flags = DRM_CASTKMS_CAPTURE_START_EXCLUSIVE,
	};
	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_START, start) < 0)
		return -errno;

	return 0;
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
	    query->reserved || query->max_registered_buffers < 4) {
		fprintf(stderr, "unexpected capture query result\n");
		return -1;
	}
	if (!(query->flags & DRM_CASTKMS_CAPTURE_CAP_IMPLICIT_SYNC)) {
		fprintf(stderr, "capture query lacks implicit sync support\n");
		return -1;
	}
	if (!(query->flags & DRM_CASTKMS_CAPTURE_CAP_SYNCOBJ_TIMELINE)) {
		fprintf(stderr, "capture query lacks timeline syncobj support\n");
		return -1;
	}

	return 0;
}


int main(int argc, char **argv)
{
	struct drm_castkms_capture_format format = {};
	struct drm_castkms_capture_query_caps query = {};
	struct drm_castkms_capture_start first_stream;
	struct drm_castkms_capture_start second_stream;
	struct drm_castkms_capture_start verifier_stream;
	uint32_t crtc_id;
	int competitor_fd = -1;
	int fd;
	int ioctl_ret;
	int ret = EXIT_FAILURE;
	int verifier_fd = -1;

	if (argc != 3) {
		fprintf(stderr, "usage: %s DRM-DEVICE CRTC-ID\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (parse_crtc_id(argv[2], &crtc_id))
		return EXIT_FAILURE;

	fd = open_capture_device(argv[1], true);
	if (fd < 0)
		return EXIT_FAILURE;

	if (require_capability(fd, DRM_CAP_SYNCOBJ, "drm_cap_syncobj") ||
	    require_capability(fd, DRM_CAP_SYNCOBJ_TIMELINE,
			       "drm_cap_syncobj_timeline"))
		goto out_close;

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

	ioctl_ret = start_capture(fd, crtc_id, &first_stream);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("start first capture stream");
		goto out_close;
	}
	if (!first_stream.stream_id || !first_stream.mode_generation) {
		fprintf(stderr, "capture start returned invalid stream metadata\n");
		goto out_close;
	}

	ret = EXIT_SUCCESS;

out_close:
	if (verifier_fd >= 0)
		close(verifier_fd);
	close(fd);
	return ret;
}
