// SPDX-License-Identifier: GPL-2.0-only

#include "castkms-test-drm.h"

#include <drm/castkms_drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

int castkms_test_check_driver_name(int fd)
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

int castkms_test_framebuffer_create(int fd, uint32_t width, uint32_t height,
				    bool map_buffer,
				    struct castkms_test_framebuffer *buffer)
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
	struct drm_mode_destroy_dumb destroy;

	*buffer = (struct castkms_test_framebuffer) {};
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

	buffer->handle = dumb.handle;
	buffer->fb_id = fb.fb_id;
	buffer->pitch = dumb.pitch;
	buffer->size = dumb.size;
	if (map_buffer) {
		struct drm_mode_map_dumb map = {
			.handle = dumb.handle,
		};

		if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
			perror("DRM_IOCTL_MODE_MAP_DUMB");
			goto err_destroy;
		}
		buffer->map = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE,
				   MAP_SHARED, fd, map.offset);
		if (buffer->map == MAP_FAILED) {
			perror("mmap dumb framebuffer");
			buffer->map = NULL;
			goto err_destroy;
		}
	}

	return 0;

err_destroy:
	castkms_test_framebuffer_destroy(fd, buffer);
	return -1;
}

void castkms_test_framebuffer_unmap(struct castkms_test_framebuffer *buffer)
{
	if (buffer->map && munmap(buffer->map, buffer->size) < 0)
		perror("munmap dumb framebuffer");
	buffer->map = NULL;
}

void castkms_test_framebuffer_destroy(
	int fd, struct castkms_test_framebuffer *buffer)
{
	struct drm_mode_destroy_dumb destroy = {
		.handle = buffer->handle,
	};

	if (fd < 0)
		return;
	castkms_test_framebuffer_unmap(buffer);
	if (buffer->fb_id && ioctl(fd, DRM_IOCTL_MODE_RMFB, &buffer->fb_id) < 0)
		perror("DRM_IOCTL_MODE_RMFB");
	if (buffer->handle &&
	    ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) < 0)
		perror("DRM_IOCTL_MODE_DESTROY_DUMB");
	*buffer = (struct castkms_test_framebuffer) {};
}

int castkms_test_capture_start(int fd, uint32_t crtc_id, uint32_t flags,
			       struct drm_castkms_capture_start *start)
{
	*start = (struct drm_castkms_capture_start) {
		.crtc_id = crtc_id,
		.flags = flags,
	};
	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_START, start) < 0)
		return -errno;

	return 0;
}

int castkms_test_capture_stop(int fd, uint32_t stream_id)
{
	struct drm_castkms_capture_stop stop = {
		.stream_id = stream_id,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_STOP, &stop) < 0)
		return -errno;

	return 0;
}

int castkms_test_capture_register_buffer(
	int fd, uint32_t stream_id, uint32_t fb_id, uint32_t flags,
	uint64_t mode_generation, uint32_t *buffer_id)
{
	struct drm_castkms_capture_register_buffer buffer = {
		.stream_id = stream_id,
		.fb_id = fb_id,
		.flags = flags,
		.mode_generation = mode_generation,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_REGISTER_BUFFER, &buffer) < 0)
		return -errno;
	*buffer_id = buffer.buffer_id;

	return 0;
}

int castkms_test_capture_unregister_buffer(int fd, uint32_t stream_id,
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

int castkms_test_capture_queue_buffer(
	int fd, uint32_t stream_id, uint32_t buffer_id, uint32_t flags,
	uint64_t mode_generation, uint64_t user_data)
{
	struct drm_castkms_capture_queue_buffer buffer = {
		.stream_id = stream_id,
		.buffer_id = buffer_id,
		.flags = flags,
		.user_data = user_data,
		.mode_generation = mode_generation,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_QUEUE_BUFFER, &buffer) < 0)
		return -errno;

	return 0;
}
