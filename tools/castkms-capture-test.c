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
static_assert(sizeof(struct drm_castkms_capture_stop) == 16,
	      "capture stop ABI size changed");
static_assert(sizeof(struct drm_castkms_capture_register_buffer) == 32,
	      "capture register ABI size changed");
static_assert(sizeof(struct drm_castkms_capture_unregister_buffer) == 16,
	      "capture unregister ABI size changed");

struct test_framebuffer {
	uint32_t handle;
	uint32_t fb_id;
	uint32_t pitch;
	uint64_t size;
	void *map;
};

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

static int stop_capture(int fd, uint32_t stream_id)
{
	struct drm_castkms_capture_stop stop = {
		.stream_id = stream_id,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_STOP, &stop) < 0)
		return -errno;

	return 0;
}

static int register_capture_buffer(int fd, uint32_t stream_id,
				   uint32_t fb_id, uint32_t flags,
				   uint32_t ready_syncobj_handle,
				   uint32_t reuse_syncobj_handle,
				   uint64_t mode_generation,
				   uint32_t *buffer_id)
{
	struct drm_castkms_capture_register_buffer buffer = {
		.stream_id = stream_id,
		.fb_id = fb_id,
		.ready_syncobj_handle = ready_syncobj_handle,
		.reuse_syncobj_handle = reuse_syncobj_handle,
		.flags = flags,
		.mode_generation = mode_generation,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_REGISTER_BUFFER, &buffer) < 0)
		return -errno;

	*buffer_id = buffer.buffer_id;
	return 0;
}

static int unregister_capture_buffer(int fd, uint32_t stream_id,
				     uint32_t buffer_id)
{
	struct drm_castkms_capture_unregister_buffer buffer = {
		.stream_id = stream_id,
		.buffer_id = buffer_id,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_UNREGISTER_BUFFER, &buffer) < 0)
		return -errno;

	return 0;
}


static int get_crtc_size(int fd, uint32_t crtc_id,
			 uint32_t *width, uint32_t *height)
{
	struct drm_mode_crtc crtc = {
		.crtc_id = crtc_id,
	};

	if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) < 0) {
		perror("DRM_IOCTL_MODE_GETCRTC");
		return -1;
	}
	if (!crtc.mode_valid || !crtc.mode.hdisplay || !crtc.mode.vdisplay) {
		fprintf(stderr, "capture CRTC has no active mode\n");
		return -1;
	}

	*width = crtc.mode.hdisplay;
	*height = crtc.mode.vdisplay;
	return 0;
}

static int create_test_framebuffer(int fd, uint32_t width, uint32_t height,
				   struct test_framebuffer *buffer)
{
	struct drm_mode_create_dumb dumb = {
		.width = width,
		.height = height,
		.bpp = 32,
	};
	struct drm_mode_fb_cmd2 fb = {
		.width = width,
		.height = height,
		.pixel_format = DRM_FORMAT_XRGB8888,
	};
	struct drm_mode_map_dumb map;
	struct drm_mode_destroy_dumb destroy;

	*buffer = (struct test_framebuffer) {};
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) < 0) {
		perror("DRM_IOCTL_MODE_CREATE_DUMB");
		return -1;
	}

	fb.handles[0] = dumb.handle;
	fb.pitches[0] = dumb.pitch;
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0) {
		perror("DRM_IOCTL_MODE_ADDFB2");
		destroy = (struct drm_mode_destroy_dumb) {
			.handle = dumb.handle,
		};
		ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
		return -1;
	}

	map = (struct drm_mode_map_dumb) {
		.handle = dumb.handle,
	};
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
		perror("DRM_IOCTL_MODE_MAP_DUMB");
		ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb.fb_id);
		destroy = (struct drm_mode_destroy_dumb) {
			.handle = dumb.handle,
		};
		ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
		return -1;
	}

	buffer->map = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE,
			   MAP_SHARED, fd, map.offset);
	if (buffer->map == MAP_FAILED) {
		perror("mmap dumb framebuffer");
		buffer->map = NULL;
		ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb.fb_id);
		destroy = (struct drm_mode_destroy_dumb) {
			.handle = dumb.handle,
		};
		ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
		return -1;
	}

	buffer->handle = dumb.handle;
	buffer->fb_id = fb.fb_id;
	buffer->pitch = dumb.pitch;
	buffer->size = dumb.size;
	return 0;
}

static void unmap_test_framebuffer(struct test_framebuffer *buffer)
{
	if (buffer->map && munmap(buffer->map, buffer->size) < 0)
		perror("munmap dumb framebuffer");
	buffer->map = NULL;
}

static void destroy_test_framebuffer(int fd, struct test_framebuffer *buffer)
{
	struct drm_mode_destroy_dumb destroy = {
		.handle = buffer->handle,
	};

	unmap_test_framebuffer(buffer);
	if (buffer->fb_id && ioctl(fd, DRM_IOCTL_MODE_RMFB, &buffer->fb_id) < 0)
		perror("DRM_IOCTL_MODE_RMFB");
	if (buffer->handle &&
	    ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) < 0)
		perror("DRM_IOCTL_MODE_DESTROY_DUMB");
	*buffer = (struct test_framebuffer) {};
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
	struct test_framebuffer competitor_buffer = {};
	struct test_framebuffer first_buffer = {};
	struct test_framebuffer wrong_size_buffer = {};
	uint32_t buffer_id;
	uint32_t crtc_id;
	uint32_t height;
	uint32_t width;
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
	if (get_crtc_size(fd, crtc_id, &width, &height) ||
	    create_test_framebuffer(fd, width, height, &first_buffer) ||
	    create_test_framebuffer(fd, width + 1, height,
				    &wrong_size_buffer))
		goto out_close;

	ioctl_ret = register_capture_buffer(fd, first_stream.stream_id,
					    first_buffer.fb_id,
		DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC, 0, 0,
		first_stream.mode_generation + 1, &buffer_id);
	if (ioctl_ret != -ESTALE) {
		fprintf(stderr,
			"stale capture buffer registration returned %d, expected %d\n",
			ioctl_ret, -ESTALE);
		goto out_close;
	}
	ioctl_ret = register_capture_buffer(fd, first_stream.stream_id,
					    wrong_size_buffer.fb_id,
		DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC, 0, 0,
		first_stream.mode_generation, &buffer_id);
	if (ioctl_ret != -EINVAL) {
		fprintf(stderr,
			"wrong-size capture buffer returned %d, expected %d\n",
			ioctl_ret, -EINVAL);
		goto out_close;
	}
	printf("capture_buffer_rejections=pass\n");

	competitor_fd = open_capture_device(argv[1], false);
	if (competitor_fd < 0)
		goto out_close;
	ioctl_ret = start_capture(competitor_fd, crtc_id, &second_stream);
	if (ioctl_ret != -EBUSY) {
		fprintf(stderr, "competing capture start returned %d, expected %d\n",
			ioctl_ret, -EBUSY);
		goto out_close;
	}
	ioctl_ret = stop_capture(competitor_fd, first_stream.stream_id);
	if (ioctl_ret != -ENOENT) {
		fprintf(stderr, "foreign capture stop returned %d, expected %d\n",
			ioctl_ret, -ENOENT);
		goto out_close;
	}
	printf("capture_stream_exclusive=pass\n");

	ioctl_ret = register_capture_buffer(fd, first_stream.stream_id,
					    first_buffer.fb_id,
		DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC, 0, 0,
		first_stream.mode_generation, &buffer_id);
	if (ioctl_ret || !buffer_id) {
		errno = ioctl_ret ? -ioctl_ret : EPROTO;
		perror("register implicit capture buffer");
		goto out_close;
	}
	ioctl_ret = unregister_capture_buffer(competitor_fd,
					      first_stream.stream_id,
					      buffer_id);
	if (ioctl_ret != -ENOENT) {
		fprintf(stderr,
			"foreign buffer unregister returned %d, expected %d\n",
			ioctl_ret, -ENOENT);
		goto out_close;
	}

	ret = EXIT_SUCCESS;

out_close:
	if (verifier_fd >= 0)
		close(verifier_fd);
	if (competitor_fd >= 0) {
		destroy_test_framebuffer(competitor_fd, &competitor_buffer);
		close(competitor_fd);
	}
	destroy_test_framebuffer(fd, &wrong_size_buffer);
	destroy_test_framebuffer(fd, &first_buffer);
	close(fd);
	return ret;
}
