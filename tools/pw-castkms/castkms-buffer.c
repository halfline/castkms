// SPDX-License-Identifier: GPL-2.0-only

#include "pw-castkms.h"

#include <drm/castkms_drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

/*
 * The DRM file has its own GEM namespace.  Every capture destination must be
 * created, registered, queued, and destroyed through that same fd.  Exporting
 * its GEM object as DMA-BUF is how this example shares the destination with
 * PipeWire without opening an ordinary card fd.
 */

static int register_destination(int fd, uint32_t stream_id, uint32_t fb_id,
				uint64_t mode_generation,
				uint32_t *buffer_id)
{
	struct drm_castkms_capture_register_buffer args = {
		.stream_id = stream_id,
		.fb_id = fb_id,
		.flags = DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC,
		.mode_generation = mode_generation,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_REGISTER_BUFFER, &args) < 0)
		return -errno;

	*buffer_id = args.buffer_id;
	return 0;
}

static int unregister_destination(int fd, uint32_t stream_id,
				  uint32_t buffer_id)
{
	struct drm_castkms_capture_unregister_buffer args = {
		.stream_id = stream_id,
		.buffer_id = buffer_id,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_UNREGISTER_BUFFER, &args) < 0)
		return -errno;

	return 0;
}

static int queue_destination(int fd, uint32_t stream_id, uint32_t buffer_id,
			     uint64_t mode_generation, uint64_t user_data)
{
	struct drm_castkms_capture_queue_buffer args = {
		.stream_id = stream_id,
		.buffer_id = buffer_id,
		.flags = DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC,
		.mode_generation = mode_generation,
		.user_data = user_data,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_QUEUE_BUFFER, &args) < 0)
		return -errno;

	return 0;
}

struct capture_buffer *
castkms_find_buffer_by_pipewire(struct pw_castkms *bridge,
			       struct pw_buffer *pipewire_buffer)
{
	uint32_t i;

	for (i = 0; i < bridge->buffer_count; i++) {
		if (bridge->buffers[i].pipewire_buffer == pipewire_buffer)
			return &bridge->buffers[i];
	}

	return NULL;
}

struct capture_buffer *
castkms_find_buffer_by_id(struct pw_castkms *bridge, uint32_t buffer_id)
{
	uint32_t i;

	for (i = 0; i < bridge->buffer_count; i++) {
		if (bridge->buffers[i].buffer_id == buffer_id)
			return &bridge->buffers[i];
	}

	return NULL;
}

int castkms_create_destination(struct pw_castkms *bridge,
			       struct capture_buffer *buffer)
{
	struct drm_mode_create_dumb dumb = {
		.width = bridge->width,
		.height = bridge->height,
		.bpp = 32,
	};
	struct drm_mode_fb_cmd2 fb = {
		.width = bridge->width,
		.height = bridge->height,
		.pixel_format = DRM_FORMAT_XRGB8888,
	};
	struct drm_prime_handle prime = {
		.flags = DRM_CLOEXEC | DRM_RDWR,
	};
	struct drm_mode_destroy_dumb destroy;
	int status;

	*buffer = (struct capture_buffer) {
		.dmabuf_fd = -1,
	};

	if (ioctl(bridge->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) < 0) {
		status = -errno;
		perror("DRM_IOCTL_MODE_CREATE_DUMB");
		return status;
	}

	buffer->gem_handle = dumb.handle;
	buffer->pitch = dumb.pitch;
	buffer->size = dumb.size;
	if (!buffer->pitch || buffer->pitch > INT32_MAX ||
	    buffer->pitch < (uint64_t)bridge->width * 4U ||
	    !buffer->size || buffer->size > UINT32_MAX ||
	    buffer->size < (uint64_t)buffer->pitch * bridge->height) {
		fprintf(stderr, "invalid dumb-buffer layout\n");
		status = -EPROTO;
		goto err_gem;
	}

	fb.handles[0] = dumb.handle;
	fb.pitches[0] = dumb.pitch;
	if (ioctl(bridge->drm_fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0) {
		status = -errno;
		perror("DRM_IOCTL_MODE_ADDFB2");
		goto err_gem;
	}
	buffer->fb_id = fb.fb_id;

	prime.handle = dumb.handle;
	if (ioctl(bridge->drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0) {
		status = -errno;
		perror("DRM_IOCTL_PRIME_HANDLE_TO_FD");
		goto err_fb;
	}
	buffer->dmabuf_fd = prime.fd;

	status = register_destination(
		bridge->drm_fd, bridge->stream_id, buffer->fb_id,
		bridge->mode_generation, &buffer->buffer_id);
	if (status) {
		errno = -status;
		perror("REGISTER_BUFFER");
		goto err_dmabuf;
	}

	buffer->state = CAPTURE_BUFFER_IN_PIPEWIRE;
	return 0;

err_dmabuf:
	close(buffer->dmabuf_fd);
	buffer->dmabuf_fd = -1;
err_fb:
	(void)ioctl(bridge->drm_fd, DRM_IOCTL_MODE_RMFB, &buffer->fb_id);
	buffer->fb_id = 0;
err_gem:
	destroy = (struct drm_mode_destroy_dumb) {
		.handle = buffer->gem_handle,
	};
	(void)ioctl(bridge->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	buffer->gem_handle = 0;
	return status;
}

int castkms_destroy_destination(struct pw_castkms *bridge,
				struct capture_buffer *buffer)
{
	struct drm_mode_destroy_dumb destroy;
	int status;

	if (buffer->buffer_id && bridge->capture_active) {
		status = unregister_destination(bridge->drm_fd,
					bridge->stream_id,
					buffer->buffer_id);
		if (status)
			return status;
	}

	if (buffer->dmabuf_fd >= 0)
		close(buffer->dmabuf_fd);

	if (buffer->fb_id)
		(void)ioctl(bridge->drm_fd, DRM_IOCTL_MODE_RMFB,
			    &buffer->fb_id);

	if (buffer->gem_handle) {
		destroy = (struct drm_mode_destroy_dumb) {
			.handle = buffer->gem_handle,
		};
		(void)ioctl(bridge->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB,
			    &destroy);
	}

	*buffer = (struct capture_buffer) {
		.dmabuf_fd = -1,
	};
	return 0;
}

void castkms_queue_available(struct pw_castkms *bridge)
{
	uint32_t i;

	if (!bridge->capture_active)
		return;

	for (i = 0; i < bridge->buffer_count; i++) {
		struct capture_buffer *buffer = &bridge->buffers[i];
		int status;

		if (buffer->state != CAPTURE_BUFFER_AVAILABLE)
			continue;

		buffer->user_data = ++bridge->user_data_sequence;
		status = queue_destination(
			bridge->drm_fd, bridge->stream_id, buffer->buffer_id,
			bridge->mode_generation, buffer->user_data);
		if (status == -EBUSY)
			break;
		if (status) {
			buffer->user_data = 0;
			pw_castkms_fail(bridge, "QUEUE_BUFFER failed", status);
			return;
		}

		buffer->state = CAPTURE_BUFFER_QUEUED;
	}
}
