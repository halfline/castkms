// SPDX-License-Identifier: GPL-2.0-only

#include <drm/castkms_drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "virtualscreen-edid.h"

#ifndef DRM_MODE_CONNECTED
#define DRM_MODE_CONNECTED 1
#define DRM_MODE_DISCONNECTED 2
#endif

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
static_assert(sizeof(struct drm_castkms_capture_queue_buffer) == 48,
	      "capture queue ABI size changed");
static_assert(sizeof(struct drm_castkms_capture_set_output_edid) == 24,
	      "capture set-output-edid ABI size changed");
static_assert(sizeof(struct drm_castkms_capture_attach_monitor) == 24,
	      "capture attach-monitor ABI size changed");
static_assert(sizeof(struct drm_castkms_capture_detach_monitor) == 16,
	      "capture detach-monitor ABI size changed");
static_assert(sizeof(struct drm_event_castkms_capture_frame) == 112,
	      "capture event ABI size changed");
static_assert(offsetof(struct drm_event_castkms_capture_frame, reserved) == 108,
	      "capture event ABI layout changed");

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

static int queue_capture_buffer(int fd, uint32_t stream_id,
				uint32_t buffer_id, uint32_t flags,
				uint64_t mode_generation, uint64_t user_data,
				uint64_t ready_point, uint64_t reuse_point)
{
	struct drm_castkms_capture_queue_buffer buffer = {
		.stream_id = stream_id,
		.buffer_id = buffer_id,
		.flags = flags,
		.user_data = user_data,
		.mode_generation = mode_generation,
		.ready_point = ready_point,
		.reuse_point = reuse_point,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_QUEUE_BUFFER, &buffer) < 0)
		return -errno;

	return 0;
}

#define TEST_EDID_BLOCK CASTKMS_EDID_BLOCK

static int fill_named_edid(uint8_t edid[TEST_EDID_BLOCK], const char *name)
{
	return castkms_fill_named_edid(edid, name);
}

static int set_output_edid(int fd, uint32_t stream_id, const void *edid,
			   uint32_t size)
{
	struct drm_castkms_capture_set_output_edid args = {
		.stream_id = stream_id,
		.edid_size = size,
		.edid_ptr = (uint64_t)(uintptr_t)edid,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_SET_OUTPUT_EDID, &args) < 0)
		return -errno;

	return 0;
}

static int attach_monitor(int fd, uint32_t connector_id, const void *edid,
			  uint32_t size)
{
	struct drm_castkms_capture_attach_monitor args = {
		.connector_id = connector_id,
		.edid_size = size,
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

static int read_connector_connection(int fd, uint32_t connector_id,
				     uint32_t *connection)
{
	struct drm_mode_get_connector conn = {
		.connector_id = connector_id,
	};

	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
		return -1;

	*connection = conn.connection;
	return 0;
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

static int read_connector_edid(int fd, uint32_t connector_id, uint8_t *edid,
			       uint32_t capacity, uint32_t *size)
{
	struct drm_mode_get_connector conn = {
		.connector_id = connector_id,
	};
	uint32_t *props = NULL;
	uint64_t *values = NULL;
	uint32_t i;
	int ret = -1;

	*size = 0;
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
		return -1;
	if (!conn.count_props)
		return 0;
	props = calloc(conn.count_props, sizeof(*props));
	values = calloc(conn.count_props, sizeof(*values));
	if (!props || !values)
		goto out;
	conn.count_modes = 0;
	conn.count_encoders = 0;
	conn.props_ptr = (uint64_t)(uintptr_t)props;
	conn.prop_values_ptr = (uint64_t)(uintptr_t)values;
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
		goto out;
	for (i = 0; i < conn.count_props; i++) {
		struct drm_mode_get_property prop = {
			.prop_id = props[i],
		};
		struct drm_mode_get_blob blob = {
			.blob_id = (uint32_t)values[i],
		};

		if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop) < 0)
			goto out;
		if (strcmp((const char *)prop.name, "EDID"))
			continue;
		if (!blob.blob_id) {
			ret = 0;
			goto out;
		}
		if (ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) < 0)
			goto out;
		if (blob.length > capacity)
			goto out;
		blob.data = (uint64_t)(uintptr_t)edid;
		if (ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) < 0)
			goto out;
		*size = blob.length;
		ret = 0;
		goto out;
	}
	ret = 0;

out:
	free(props);
	free(values);
	return ret;
}

static int
queue_capture_when_available(int fd, uint32_t stream_id, uint32_t buffer_id,
			     uint32_t flags, uint64_t mode_generation,
			     uint64_t user_data, uint64_t ready_point,
			     uint64_t reuse_point)
{
	const struct timespec retry_delay = {
		.tv_nsec = 1000000,
	};
	struct timespec deadline;
	struct timespec now;
	int ret;

	if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0)
		return -errno;
	deadline.tv_sec += 2;

	for (;;) {
		ret = queue_capture_buffer(fd, stream_id, buffer_id, flags,
					   mode_generation, user_data,
					   ready_point, reuse_point);
		if (ret != -EBUSY)
			return ret;
		if (nanosleep(&retry_delay, NULL) < 0)
			return -errno;
		if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
			return -errno;
		if (now.tv_sec > deadline.tv_sec ||
		    (now.tv_sec == deadline.tv_sec &&
		     now.tv_nsec >= deadline.tv_nsec))
			return -ETIMEDOUT;
	}
}

static int read_capture_event_timeout(int fd,
				      struct drm_event_castkms_capture_frame *event,
				      int timeout_ms)
{
	struct pollfd poll_fd = {
		.fd = fd,
		.events = POLLIN,
	};
	ssize_t length;
	int ret;

	ret = poll(&poll_fd, 1, timeout_ms);
	if (ret < 0) {
		perror("poll capture event");
		return -1;
	}
	if (!ret || !(poll_fd.revents & POLLIN)) {
		fprintf(stderr, "timed out waiting for capture event\n");
		return -1;
	}

	length = read(fd, event, sizeof(*event));
	if (length < 0) {
		perror("read capture event");
		return -1;
	}
	if (length != sizeof(*event)) {
		fprintf(stderr, "unexpected capture event length: %zd\n", length);
		return -1;
	}

	return 0;
}

static int read_capture_event(int fd,
			      struct drm_event_castkms_capture_frame *event)
{
	return read_capture_event_timeout(fd, event, 2000);
}

static int
validate_capture_event(const struct drm_event_castkms_capture_frame *event,
		       uint32_t stream_id, uint32_t buffer_id,
		       uint64_t user_data, uint64_t mode_generation,
		       uint32_t width, uint32_t height, uint64_t after_sequence,
		       bool may_have_dropped_frames)
{
	if (event->base.type != DRM_CASTKMS_CAPTURE_EVENT_FRAME ||
	    event->base.length != sizeof(*event) ||
	    event->user_data != user_data || event->stream_id != stream_id ||
	    event->buffer_id != buffer_id || event->status ||
	    event->flags & ~(uint32_t)DRM_CASTKMS_CAPTURE_FRAME_FULL_DAMAGE ||
	    event->sequence <= after_sequence ||
	    ((!may_have_dropped_frames && event->dropped_frames) ||
	     (may_have_dropped_frames &&
	      event->dropped_frames > event->sequence - after_sequence - 1)) ||
	    event->timestamp_ns <= 0 ||
	    event->mode_generation != mode_generation ||
	    event->damage_x < 0 || event->damage_y < 0 ||
	    !event->damage_width || !event->damage_height ||
	    (uint32_t)event->damage_x + event->damage_width > width ||
	    (uint32_t)event->damage_y + event->damage_height > height ||
	    event->reserved) {
		fprintf(stderr, "unexpected capture completion event\n");
		return -1;
	}

	return 0;
}

static int
validate_capture_damage(const struct drm_event_castkms_capture_frame *event,
			uint32_t width, uint32_t height)
{
	bool full_damage = !!(event->flags & DRM_CASTKMS_CAPTURE_FRAME_FULL_DAMAGE);

	if (event->damage_x < 0 || event->damage_y < 0 ||
	    !event->damage_width || !event->damage_height) {
		fprintf(stderr, "damage rect is empty or has negative origin\n");
		return -1;
	}

	if ((uint32_t)event->damage_x + event->damage_width > width ||
	    (uint32_t)event->damage_y + event->damage_height > height) {
		fprintf(stderr,
			"damage rect exceeds frame bounds: (%d,%d)+%ux%u in %ux%u\n",
			event->damage_x, event->damage_y,
			event->damage_width, event->damage_height,
			width, height);
		return -1;
	}

	if (full_damage) {
		if (event->damage_x != 0 || event->damage_y != 0 ||
		    event->damage_width != width ||
		    event->damage_height != height) {
			fprintf(stderr,
				"FULL_DAMAGE flag set but rect is not full frame: "
				"(%d,%d)+%ux%u vs %ux%u\n",
				event->damage_x, event->damage_y,
				event->damage_width, event->damage_height,
				width, height);
			return -1;
		}
	}

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
	if (!crtc.mode_valid || !crtc.mode.hdisplay || !crtc.mode.vdisplay)
		return -1;

	*width = crtc.mode.hdisplay;
	*height = crtc.mode.vdisplay;
	return 0;
}

static int wait_crtc_size(int fd, uint32_t crtc_id,
			  uint32_t *width, uint32_t *height)
{
	const struct timespec retry_delay = {
		.tv_nsec = 50000000,
	};
	int attempt;

	for (attempt = 0; attempt < 100; attempt++) {
		if (!get_crtc_size(fd, crtc_id, width, height))
			return 0;
		if (nanosleep(&retry_delay, NULL) < 0)
			return -1;
	}

	fprintf(stderr, "capture CRTC has no active mode after attach\n");
	return -1;
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

static int export_framebuffer_dmabuf(int fd, uint32_t handle)
{
	struct drm_prime_handle prime = {
		.handle = handle,
		.flags = DRM_CLOEXEC | DRM_RDWR,
	};

	if (ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0) {
		perror("DRM_IOCTL_PRIME_HANDLE_TO_FD");
		return -1;
	}

	return prime.fd;
}

static int export_write_fence(int dmabuf_fd)
{
	struct dma_buf_export_sync_file export = {
		.flags = DMA_BUF_SYNC_WRITE,
		.fd = -1,
	};

	if (ioctl(dmabuf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export) < 0) {
		perror("DMA_BUF_IOCTL_EXPORT_SYNC_FILE");
		return -1;
	}

	return export.fd;
}

static int read_sync_file_info(int fence_fd, struct sync_file_info *info,
			       struct sync_fence_info **fences)
{
	*info = (struct sync_file_info) {};
	*fences = NULL;
	if (ioctl(fence_fd, SYNC_IOC_FILE_INFO, info) < 0) {
		perror("SYNC_IOC_FILE_INFO count");
		return -1;
	}
	if (!info->num_fences) {
		fprintf(stderr, "capture sync file contains no fences\n");
		return -1;
	}

	*fences = calloc(info->num_fences, sizeof(**fences));
	if (!*fences) {
		perror("calloc sync-file fences");
		return -1;
	}
	info->sync_fence_info = (uint64_t)(uintptr_t)*fences;
	if (ioctl(fence_fd, SYNC_IOC_FILE_INFO, info) < 0) {
		perror("SYNC_IOC_FILE_INFO fences");
		free(*fences);
		*fences = NULL;
		return -1;
	}

	return 0;
}

static bool capture_fence_name_is_valid(const struct sync_fence_info *fence)
{
	return (!strcmp(fence->driver_name, "castkms") &&
		!strcmp(fence->obj_name, "capture")) ||
	       (!strcmp(fence->driver_name, "detached-driver") &&
		!strcmp(fence->obj_name, "signaled-timeline"));
}

static int validate_queued_capture_fence(int fence_fd, bool *pending)
{
	struct sync_fence_info *fences;
	struct sync_file_info info;
	int ret = -1;

	if (read_sync_file_info(fence_fd, &info, &fences))
		return -1;
	*pending = false;
	if (info.num_fences != 1 || info.status < 0 ||
	    !capture_fence_name_is_valid(&fences[0]) ||
	    fences[0].status != info.status) {
		fprintf(stderr, "unexpected queued capture fence\n");
	} else if (!info.status &&
		 (!strcmp(fences[0].driver_name, "castkms") &&
		  !strcmp(fences[0].obj_name, "capture"))) {
		*pending = true;
		ret = 0;
	} else if (info.status == 1 && fences[0].timestamp_ns) {
		ret = 0;
	} else {
		fprintf(stderr, "queued capture fence has invalid state\n");
	}

	free(fences);
	return ret;
}

static int import_read_fence(int dmabuf_fd, int fence_fd)
{
	struct dma_buf_import_sync_file import = {
		.flags = DMA_BUF_SYNC_READ,
		.fd = fence_fd,
	};

	if (ioctl(dmabuf_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &import) < 0) {
		perror("DMA_BUF_IOCTL_IMPORT_SYNC_FILE");
		return -1;
	}

	return 0;
}

static int inspect_capture_fence(int fence_fd, bool *pending)
{
	struct sync_fence_info *fences;
	struct sync_file_info info;
	bool active_capture = false;
	bool valid = true;

	if (read_sync_file_info(fence_fd, &info, &fences))
		return -1;
	for (uint32_t i = 0; i < info.num_fences; i++) {
		if (!strcmp(fences[i].driver_name, "castkms") &&
		    !strcmp(fences[i].obj_name, "capture") &&
		    !fences[i].status)
			active_capture = true;
		if (fences[i].status < 0 ||
		    !capture_fence_name_is_valid(&fences[i]) ||
		    (fences[i].status == 1 && !fences[i].timestamp_ns))
			valid = false;
	}

	free(fences);
	if (info.status < 0 || !info.num_fences || !valid ||
	    (!info.status && !active_capture)) {
		fprintf(stderr,
			"dependent capture fence has invalid state: status=%d fences=%u active=%d\n",
			info.status, info.num_fences, active_capture);
		return -1;
	}
	*pending = !info.status;

	return 0;
}

static int wait_for_capture_fence_timeout(int fence_fd, int timeout_ms)
{
	struct sync_fence_info *fences;
	struct sync_file_info info;
	struct pollfd poll_fd = {
		.fd = fence_fd,
		.events = POLLIN,
	};
	int ret;

	ret = poll(&poll_fd, 1, timeout_ms);
	if (ret < 0) {
		perror("poll capture fence");
		return -1;
	}
	if (!ret || !(poll_fd.revents & POLLIN)) {
		fprintf(stderr, "timed out waiting for capture fence\n");
		return -1;
	}

	if (read_sync_file_info(fence_fd, &info, &fences))
		return -1;
	ret = info.status == 1 ? 0 : -1;
	for (uint32_t i = 0; i < info.num_fences; i++)
		if (fences[i].status != 1 || !fences[i].timestamp_ns ||
		    !capture_fence_name_is_valid(&fences[i]))
			ret = -1;
	free(fences);

	if (ret)
		fprintf(stderr, "capture producer fence did not complete\n");
	return ret;
}

static int wait_for_capture_fence(int fence_fd)
{
	return wait_for_capture_fence_timeout(fence_fd, 2000);
}

static int wait_for_capture_fence_error(int fence_fd, int expected_status)
{
	struct sync_fence_info *fences;
	struct sync_file_info info;
	struct pollfd poll_fd = {
		.fd = fence_fd,
		.events = POLLIN,
	};
	bool found = false;
	int ret;

	ret = poll(&poll_fd, 1, 2000);
	if (ret <= 0 || !(poll_fd.revents & POLLIN)) {
		fprintf(stderr, "timed out waiting for cancelled capture fence\n");
		return -1;
	}
	if (read_sync_file_info(fence_fd, &info, &fences))
		return -1;

	ret = info.status == expected_status ? 0 : -1;
	for (uint32_t i = 0; i < info.num_fences; i++) {
		if (fences[i].status == expected_status)
			found = true;
		else if (fences[i].status != 1)
			ret = -1;
		if (!capture_fence_name_is_valid(&fences[i]) ||
		    !fences[i].timestamp_ns)
			ret = -1;
	}
	free(fences);

	if (ret || !found) {
		fprintf(stderr,
			"capture fence status=%d, expected cancellation status=%d\n",
			info.status, expected_status);
		return -1;
	}

	return 0;
}

static int sync_dmabuf_cpu_access(int dmabuf_fd, uint64_t flags)
{
	struct dma_buf_sync sync = {
		.flags = flags,
	};

	if (ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) {
		perror("DMA_BUF_IOCTL_SYNC");
		return -1;
	}

	return 0;
}

static int create_syncobj(int fd, uint32_t *handle)
{
	struct drm_syncobj_create create = {};

	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create) < 0) {
		perror("DRM_IOCTL_SYNCOBJ_CREATE");
		return -1;
	}

	*handle = create.handle;
	return 0;
}

static void destroy_syncobj(int fd, uint32_t *handle)
{
	struct drm_syncobj_destroy destroy = {
		.handle = *handle,
	};

	if (*handle && ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy) < 0)
		perror("DRM_IOCTL_SYNCOBJ_DESTROY");
	*handle = 0;
}

static int transfer_syncobj_point(int fd, uint32_t source_handle,
				  uint64_t source_point,
				  uint32_t destination_handle,
				  uint64_t destination_point)
{
	struct drm_syncobj_transfer transfer = {
		.src_handle = source_handle,
		.dst_handle = destination_handle,
		.src_point = source_point,
		.dst_point = destination_point,
	};

	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &transfer) < 0) {
		perror("DRM_IOCTL_SYNCOBJ_TRANSFER");
		return -1;
	}

	return 0;
}

static int signal_syncobj_point(int fd, uint32_t handle, uint64_t point)
{
	struct drm_syncobj_timeline_array signal = {
		.handles = (uint64_t)(uintptr_t)&handle,
		.points = (uint64_t)(uintptr_t)&point,
		.count_handles = 1,
	};

	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &signal) < 0) {
		perror("DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL");
		return -1;
	}

	return 0;
}

static int wait_syncobj_point(int fd, uint32_t handle, uint64_t point,
			      uint32_t flags, int64_t timeout_ns)
{
	struct drm_syncobj_timeline_wait wait = {
		.handles = (uint64_t)(uintptr_t)&handle,
		.points = (uint64_t)(uintptr_t)&point,
		.timeout_nsec = timeout_ns,
		.count_handles = 1,
		.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | flags,
	};

	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &wait) < 0)
		return -errno;

	return 0;
}

static int syncobj_point_is_available(int fd, uint32_t handle, uint64_t point)
{
	int ret;

	ret = wait_syncobj_point(fd, handle, point,
				 DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, 0);
	if (ret) {
		errno = -ret;
		perror("wait for syncobj point availability");
		return -1;
	}

	return 0;
}

static int inspect_syncobj_point(int fd, uint32_t handle, uint64_t point,
				 bool *pending)
{
	int ret;

	if (syncobj_point_is_available(fd, handle, point))
		return -1;

	ret = wait_syncobj_point(fd, handle, point, 0, 0);
	if (!ret) {
		*pending = false;
		return 0;
	}
	if (ret != -ETIME) {
		errno = -ret;
		fprintf(stderr, "syncobj point has invalid state: %s\n",
			strerror(errno));
		return -1;
	}
	*pending = true;

	return 0;
}

static int wait_for_signaled_syncobj_point(int fd, uint32_t handle,
					   uint64_t point)
{
	struct timespec now;
	int64_t deadline_ns;
	int ret;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
		perror("clock_gettime");
		return -1;
	}
	deadline_ns = (int64_t)now.tv_sec * INT64_C(1000000000) +
		      now.tv_nsec + INT64_C(2000000000);
	ret = wait_syncobj_point(fd, handle, point, 0, deadline_ns);
	if (ret) {
		errno = -ret;
		perror("ready point did not follow its completion event");
		return -1;
	}

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

static int run_attach_hold(const char *device, uint32_t crtc_id)
{
	uint8_t edid[TEST_EDID_BLOCK];
	uint32_t connector_id;
	uint32_t connection;
	char discard;
	int fd;
	int ret = EXIT_FAILURE;

	fd = open_capture_device(device, false);
	if (fd < 0)
		return EXIT_FAILURE;

	if (find_display_connector(fd, crtc_id, &connector_id)) {
		fprintf(stderr, "failed to find display connector\n");
		goto out_close;
	}
	if (fill_named_edid(edid, NULL)) {
		fprintf(stderr, "failed to build attach EDID\n");
		goto out_close;
	}
	if (attach_monitor(fd, connector_id, edid, sizeof(edid))) {
		perror("ATTACH_MONITOR");
		goto out_close;
	}
	if (read_connector_connection(fd, connector_id, &connection) ||
	    connection != DRM_MODE_CONNECTED) {
		fprintf(stderr, "attached connector is not connected\n");
		goto out_detach;
	}

	printf("connector_id=%u\n", connector_id);
	printf("attached=1\n");
	fflush(stdout);

	/*
	 * Stop on the first stdin byte or EOF. A fifo opened RDWR in the
	 * parent never delivers EOF while this process also holds it.
	 */
	(void)read(STDIN_FILENO, &discard, 1);

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

static int run_deliver_one(const char *device, uint32_t crtc_id)
{
	const uint64_t user_data = 0x434153544b4d5305ULL;
	struct drm_event_castkms_capture_frame event = {};
	struct drm_castkms_capture_start stream;
	struct test_framebuffer buffer = {};
	uint32_t buffer_id = 0;
	uint32_t height;
	uint32_t width;
	bool fence_pending;
	int dmabuf_fd = -1;
	int fd;
	int fence_fd = -1;
	int ioctl_ret;
	int ret = EXIT_FAILURE;

	fd = open_capture_device(device, false);
	if (fd < 0)
		return EXIT_FAILURE;

	ioctl_ret = start_capture(fd, crtc_id, &stream);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("start overlap capture stream");
		goto out_close;
	}
	if (!stream.stream_id || !stream.mode_generation) {
		fprintf(stderr,
			"overlap capture start returned invalid stream metadata\n");
		goto out_close;
	}
	if (get_crtc_size(fd, crtc_id, &width, &height) ||
	    create_test_framebuffer(fd, width, height, &buffer))
		goto out_close;
	dmabuf_fd = export_framebuffer_dmabuf(fd, buffer.handle);
	if (dmabuf_fd < 0)
		goto out_close;

	if (sync_dmabuf_cpu_access(dmabuf_fd,
				   DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE))
		goto out_close;
	memset(buffer.map, 0x77, buffer.size);
	if (sync_dmabuf_cpu_access(dmabuf_fd,
				   DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE))
		goto out_close;

	ioctl_ret = register_capture_buffer(fd, stream.stream_id, buffer.fb_id,
		DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC, 0, 0,
		stream.mode_generation, &buffer_id);
	if (ioctl_ret || !buffer_id) {
		errno = ioctl_ret ? -ioctl_ret : EPROTO;
		perror("register overlap capture buffer");
		goto out_close;
	}

	ioctl_ret = queue_capture_buffer(fd, stream.stream_id, buffer_id,
					 DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC,
					 stream.mode_generation, user_data,
					 0, 0);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("queue overlap capture buffer");
		goto out_close;
	}
	printf("capture_overlap_queued=1\n");
	fflush(stdout);

	fence_fd = export_write_fence(dmabuf_fd);
	if (fence_fd < 0)
		goto out_close;
	if (validate_queued_capture_fence(fence_fd, &fence_pending))
		goto out_close;
	if (read_capture_event_timeout(fd, &event, 8000))
		goto out_close;
	if (wait_for_capture_fence_timeout(fence_fd, 8000))
		goto out_close;
	if (validate_capture_event(&event, stream.stream_id, buffer_id,
				   user_data, stream.mode_generation, width,
				   height, 0, true))
		goto out_close;
	if (validate_capture_damage(&event, width, height))
		goto out_close;
	if (sync_dmabuf_cpu_access(dmabuf_fd,
				   DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ))
		goto out_close;
	if (*(const uint32_t *)buffer.map == UINT32_C(0x77777777)) {
		fprintf(stderr,
			"overlap capture did not update its destination\n");
		goto out_close;
	}
	if (sync_dmabuf_cpu_access(dmabuf_fd,
				   DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ))
		goto out_close;

	ioctl_ret = stop_capture(fd, stream.stream_id);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("stop overlap capture stream");
		goto out_close;
	}
	printf("capture_writeback_overlap=pass\n");
	ret = EXIT_SUCCESS;

out_close:
	if (fence_fd >= 0)
		close(fence_fd);
	if (dmabuf_fd >= 0)
		close(dmabuf_fd);
	destroy_test_framebuffer(fd, &buffer);
	close(fd);
	return ret;
}

int main(int argc, char **argv)
{
	const uint64_t capture_user_data = 0x434153544b4d5304ULL;
	struct drm_event_castkms_capture_frame capture_event = {};
	struct drm_castkms_capture_format format = {};
	struct drm_castkms_capture_query_caps query = {};
	struct drm_castkms_capture_start first_stream;
	struct drm_castkms_capture_start second_stream;
	struct drm_castkms_capture_start verifier_stream;
	struct test_framebuffer competitor_buffer = {};
	struct test_framebuffer first_buffer = {};
	struct test_framebuffer second_buffer = {};
	struct test_framebuffer wrong_size_buffer = {};
	uint32_t extra_buffer_ids[6] = {};
	uint32_t buffer_id;
	uint32_t competitor_ready_syncobj = 0;
	uint32_t competitor_reuse_syncobj = 0;
	uint32_t crtc_id;
	uint32_t height;
	uint32_t ready_syncobj = 0;
	uint32_t reuse_syncobj = 0;
	uint32_t second_buffer_id;
	uint32_t second_ready_syncobj = 0;
	uint32_t second_reuse_syncobj = 0;
	uint32_t width;
	uint64_t explicit_sequence;
	uint64_t first_sequence;
	bool capture_fence_pending;
	bool dependent_fence_pending;
	bool explicit_wait_observed = false;
	bool implicit_wait_observed = false;
	bool pixel_changed;
	int capture_fence_fd = -1;
	int competitor_fd = -1;
	int dmabuf_fd = -1;
	int fd;
	int ioctl_ret;
	int ret = EXIT_FAILURE;
	int second_dmabuf_fd = -1;
	int second_fence_fd = -1;
	int verifier_fd = -1;

	if (argc == 4 && !strcmp(argv[1], "--deliver-one")) {
		if (parse_crtc_id(argv[3], &crtc_id))
			return EXIT_FAILURE;
		return run_deliver_one(argv[2], crtc_id);
	}
	if (argc == 4 && !strcmp(argv[1], "--attach")) {
		if (parse_crtc_id(argv[3], &crtc_id))
			return EXIT_FAILURE;
		return run_attach_hold(argv[2], crtc_id);
	}
	if (argc == 4 && !strcmp(argv[1], "--mode-generation")) {
		struct drm_castkms_capture_start stream;

		if (parse_crtc_id(argv[3], &crtc_id))
			return EXIT_FAILURE;
		fd = open_capture_device(argv[2], false);
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
		stop_capture(fd, stream.stream_id);
		close(fd);
		return EXIT_SUCCESS;
	}
	if (argc != 3) {
		fprintf(stderr,
			"usage: %s [--deliver-one|--attach|--mode-generation] DRM-DEVICE CRTC-ID\n",
			argv[0]);
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

	{
		uint8_t edid[TEST_EDID_BLOCK];
		uint8_t observed[512];
		uint32_t connector_id;
		uint32_t connection;
		uint32_t observed_size;

		if (fill_named_edid(edid, "CastKMS Test")) {
			fprintf(stderr, "failed to build test EDID\n");
			goto out_close;
		}
		if (find_display_connector(fd, crtc_id, &connector_id)) {
			fprintf(stderr, "failed to find display connector\n");
			goto out_close;
		}
		if (read_connector_connection(fd, connector_id, &connection) ||
		    connection != DRM_MODE_DISCONNECTED) {
			fprintf(stderr,
				"display connector is not disconnected at rest\n");
			goto out_close;
		}
		ioctl_ret = set_output_edid(fd, first_stream.stream_id, edid,
					    sizeof(edid));
		if (ioctl_ret != -ENOTCONN) {
			fprintf(stderr,
				"EDID without attach returned %d, expected %d\n",
				ioctl_ret, -ENOTCONN);
			goto out_close;
		}
		ioctl_ret = attach_monitor(fd, connector_id, NULL, 0);
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("attach monitor");
			goto out_close;
		}
		if (read_connector_connection(fd, connector_id, &connection) ||
		    connection != DRM_MODE_CONNECTED) {
			fprintf(stderr, "attached connector is not connected\n");
			goto out_close;
		}
		ioctl_ret = attach_monitor(fd, connector_id, NULL, 0);
		if (ioctl_ret != -EBUSY) {
			fprintf(stderr,
				"second attach returned %d, expected %d\n",
				ioctl_ret, -EBUSY);
			goto out_close;
		}
		ioctl_ret = attach_monitor(competitor_fd, connector_id, NULL, 0);
		if (ioctl_ret != -EBUSY) {
			fprintf(stderr,
				"foreign attach returned %d, expected %d\n",
				ioctl_ret, -EBUSY);
			goto out_close;
		}
		ioctl_ret = detach_monitor(competitor_fd, connector_id);
		if (ioctl_ret != -EACCES) {
			fprintf(stderr,
				"foreign detach returned %d, expected %d\n",
				ioctl_ret, -EACCES);
			goto out_close;
		}
		printf("capture_attach_monitor=pass\n");
		if (wait_crtc_size(fd, crtc_id, &width, &height))
			goto out_close;
		ioctl_ret = stop_capture(fd, first_stream.stream_id);
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("stop capture before modeset restart");
			goto out_close;
		}
		ioctl_ret = start_capture(fd, crtc_id, &first_stream);
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("restart capture after attach modeset");
			goto out_close;
		}
		ioctl_ret = set_output_edid(fd, first_stream.stream_id, edid,
					    sizeof(edid));
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("set output EDID");
			goto out_close;
		}
		if (read_connector_edid(fd, connector_id, observed,
					sizeof(observed), &observed_size) ||
		    observed_size != sizeof(edid) ||
		    memcmp(observed, edid, sizeof(edid))) {
			fprintf(stderr,
				"published EDID was not visible on the connector\n");
			goto out_close;
		}
		edid[TEST_EDID_BLOCK - 1] ^= 0xff;
		ioctl_ret = set_output_edid(fd, first_stream.stream_id, edid,
					    sizeof(edid));
		if (ioctl_ret != -EINVAL) {
			fprintf(stderr,
				"bad EDID checksum returned %d, expected %d\n",
				ioctl_ret, -EINVAL);
			goto out_close;
		}
		if (fill_named_edid(edid, "CastKMS Test")) {
			fprintf(stderr, "failed to rebuild test EDID\n");
			goto out_close;
		}
		ioctl_ret = set_output_edid(fd, first_stream.stream_id, edid,
					    100);
		if (ioctl_ret != -EINVAL) {
			fprintf(stderr,
				"bad EDID size returned %d, expected %d\n",
				ioctl_ret, -EINVAL);
			goto out_close;
		}
		ioctl_ret = set_output_edid(competitor_fd,
					    first_stream.stream_id, edid,
					    sizeof(edid));
		if (ioctl_ret != -ENOENT) {
			fprintf(stderr,
				"foreign EDID set returned %d, expected %d\n",
				ioctl_ret, -ENOENT);
			goto out_close;
		}
		ioctl_ret = set_output_edid(fd, first_stream.stream_id, NULL, 0);
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("clear output EDID");
			goto out_close;
		}
		if (read_connector_edid(fd, connector_id, observed,
					sizeof(observed), &observed_size) ||
		    observed_size) {
			fprintf(stderr,
				"cleared EDID remained on the connector\n");
			goto out_close;
		}
		ioctl_ret = set_output_edid(fd, first_stream.stream_id, edid,
					    sizeof(edid));
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("restore output EDID");
			goto out_close;
		}
		printf("capture_output_edid=pass\n");
	}

	if (create_test_framebuffer(fd, width, height, &first_buffer) ||
	    create_test_framebuffer(fd, width, height, &second_buffer) ||
	    create_test_framebuffer(fd, width + 1, height,
				    &wrong_size_buffer))
		goto out_close;
	dmabuf_fd = export_framebuffer_dmabuf(fd, first_buffer.handle);
	if (dmabuf_fd < 0)
		goto out_close;
	second_dmabuf_fd = export_framebuffer_dmabuf(fd, second_buffer.handle);
	if (second_dmabuf_fd < 0)
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
	ioctl_ret = register_capture_buffer(fd, first_stream.stream_id,
					    first_buffer.fb_id,
		DRM_CASTKMS_CAPTURE_BUFFER_EXPLICIT_SYNC, UINT32_MAX,
		UINT32_MAX, first_stream.mode_generation, &buffer_id);
	if (ioctl_ret != -ENOENT) {
		fprintf(stderr,
			"unknown capture syncobjs returned %d, expected %d\n",
			ioctl_ret, -ENOENT);
		goto out_close;
	}
	printf("capture_buffer_rejections=pass\n");

	ioctl_ret = register_capture_buffer(fd, first_stream.stream_id,
					    first_buffer.fb_id,
		DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC, 0, 0,
		first_stream.mode_generation, &buffer_id);
	if (ioctl_ret || !buffer_id) {
		errno = ioctl_ret ? -ioctl_ret : EPROTO;
		perror("register implicit capture buffer");
		goto out_close;
	}
	ioctl_ret = register_capture_buffer(fd, first_stream.stream_id,
					    second_buffer.fb_id,
		DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC, 0, 0,
		first_stream.mode_generation, &second_buffer_id);
	if (ioctl_ret || !second_buffer_id) {
		errno = ioctl_ret ? -ioctl_ret : EPROTO;
		perror("register second implicit capture buffer");
		goto out_close;
	}
	for (size_t i = 0; i < sizeof(extra_buffer_ids) /
					      sizeof(extra_buffer_ids[0]); i++) {
		ioctl_ret = register_capture_buffer(fd, first_stream.stream_id,
			first_buffer.fb_id,
			DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC, 0, 0,
			first_stream.mode_generation, &extra_buffer_ids[i]);
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("register buffer up to advertised limit");
			goto out_close;
		}
	}
	{
		uint32_t rejected_id = 0;

		ioctl_ret = register_capture_buffer(fd, first_stream.stream_id,
			first_buffer.fb_id,
			DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC, 0, 0,
			first_stream.mode_generation, &rejected_id);
		if (ioctl_ret != -ENOSPC) {
			fprintf(stderr,
				"ninth capture buffer returned %d, expected %d\n",
				ioctl_ret, -ENOSPC);
			goto out_close;
		}
	}
	for (size_t i = 0; i < sizeof(extra_buffer_ids) /
					      sizeof(extra_buffer_ids[0]); i++) {
		ioctl_ret = unregister_capture_buffer(fd, first_stream.stream_id,
						      extra_buffer_ids[i]);
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("unregister buffer-limit probe");
			goto out_close;
		}
		extra_buffer_ids[i] = 0;
	}
	printf("capture_buffer_limit=pass\n");
	ioctl_ret = unregister_capture_buffer(competitor_fd,
					      first_stream.stream_id,
					      buffer_id);
	if (ioctl_ret != -ENOENT) {
		fprintf(stderr,
			"foreign buffer unregister returned %d, expected %d\n",
			ioctl_ret, -ENOENT);
		goto out_close;
	}
	ioctl_ret =
		queue_capture_buffer(fd, first_stream.stream_id, buffer_id,
				     DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC,
				     first_stream.mode_generation + 1,
				     capture_user_data, 0, 0);
	if (ioctl_ret != -ESTALE) {
		fprintf(stderr,
			"stale capture queue returned %d, expected %d\n",
			ioctl_ret, -ESTALE);
		goto out_close;
	}
	ioctl_ret =
		queue_capture_buffer(fd, first_stream.stream_id, buffer_id,
				     DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC,
				     first_stream.mode_generation,
				     capture_user_data, 1, 0);
	if (ioctl_ret != -EINVAL) {
		fprintf(stderr,
			"implicit capture point returned %d, expected %d\n",
			ioctl_ret, -EINVAL);
		goto out_close;
	}

	if (sync_dmabuf_cpu_access(dmabuf_fd,
				   DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE))
		goto out_close;
	memset(first_buffer.map, 0x77, first_buffer.size);
	if (sync_dmabuf_cpu_access(dmabuf_fd,
				   DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE))
		goto out_close;
	if (sync_dmabuf_cpu_access(second_dmabuf_fd,
				   DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE))
		goto out_close;
	memset(second_buffer.map, 0x77, second_buffer.size);
	if (sync_dmabuf_cpu_access(second_dmabuf_fd,
				   DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE))
		goto out_close;
	ioctl_ret =
		queue_capture_buffer(fd, first_stream.stream_id, buffer_id,
				     DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC,
				     first_stream.mode_generation,
				     capture_user_data, 0, 0);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("queue implicit capture buffer");
		goto out_close;
	}
	capture_fence_fd = export_write_fence(dmabuf_fd);
	if (capture_fence_fd < 0)
		goto out_close;
	if (validate_queued_capture_fence(capture_fence_fd,
					  &capture_fence_pending))
		goto out_close;
	if (import_read_fence(second_dmabuf_fd, capture_fence_fd))
		goto out_close;
	ioctl_ret = queue_capture_when_available(fd, first_stream.stream_id,
						 second_buffer_id,
					 DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC,
					 first_stream.mode_generation,
					 capture_user_data + 1, 0, 0);
	if (ioctl_ret) {
		fprintf(stderr,
			"queue dependent capture buffer returned %d\n",
			ioctl_ret);
		goto out_close;
	}
	ioctl_ret = queue_capture_buffer(fd, first_stream.stream_id, buffer_id,
					 DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC,
					 first_stream.mode_generation,
					 capture_user_data + 2, 0, 0);
	if (ioctl_ret != -EBUSY) {
		fprintf(stderr,
			"queue with queued/in-flight work returned %d, expected %d\n",
			ioctl_ret, -EBUSY);
		goto out_close;
	}
	printf("capture_buffer_busy=pass\n");
	if (validate_queued_capture_fence(capture_fence_fd,
					  &capture_fence_pending))
		goto out_close;
	implicit_wait_observed = capture_fence_pending;
	second_fence_fd = export_write_fence(second_dmabuf_fd);
	if (second_fence_fd < 0 ||
	    inspect_capture_fence(second_fence_fd, &dependent_fence_pending))
		goto out_close;
	if (wait_for_capture_fence(capture_fence_fd))
		goto out_close;
	ioctl_ret = unregister_capture_buffer(fd, first_stream.stream_id,
					      buffer_id);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("unregister after implicit producer fence");
		goto out_close;
	}
	if (read_capture_event(fd, &capture_event))
		goto out_close;
	close(capture_fence_fd);
	capture_fence_fd = -1;
	if (validate_capture_event(&capture_event, first_stream.stream_id,
				   buffer_id, capture_user_data,
				   first_stream.mode_generation, width, height,
				   0, false))
		goto out_close;
	if (validate_capture_damage(&capture_event, width, height))
		goto out_close;
	if (!(capture_event.flags & DRM_CASTKMS_CAPTURE_FRAME_FULL_DAMAGE)) {
		fprintf(stderr,
			"initial capture expected full damage from legacy modeset\n");
		goto out_close;
	}
	if (sync_dmabuf_cpu_access(dmabuf_fd,
				   DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ))
		goto out_close;
	pixel_changed =
		*(const uint32_t *)first_buffer.map != UINT32_C(0x77777777);
	if (sync_dmabuf_cpu_access(dmabuf_fd,
				   DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ))
		goto out_close;
	if (!pixel_changed) {
		fprintf(stderr, "capture did not update its destination\n");
		goto out_close;
	}
	first_sequence = capture_event.sequence;
	if (wait_for_capture_fence(second_fence_fd))
		goto out_close;
	ioctl_ret = unregister_capture_buffer(fd, first_stream.stream_id,
					      second_buffer_id);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("unregister after dependent producer fence");
		goto out_close;
	}
	if (read_capture_event(fd, &capture_event))
		goto out_close;
	if (validate_capture_event(&capture_event, first_stream.stream_id,
				   second_buffer_id, capture_user_data + 1,
				   first_stream.mode_generation, width, height,
				   first_sequence, true))
		goto out_close;
	close(second_fence_fd);
	second_fence_fd = -1;
	if (sync_dmabuf_cpu_access(second_dmabuf_fd,
				   DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ))
		goto out_close;
	pixel_changed =
		*(const uint32_t *)second_buffer.map != UINT32_C(0x77777777);
	if (sync_dmabuf_cpu_access(second_dmabuf_fd,
				   DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ))
		goto out_close;
	if (!pixel_changed) {
		fprintf(stderr, "dependent capture did not update its destination\n");
		goto out_close;
	}
	printf("capture_reuse_dependency=pass\n");
	printf("capture_reuse_wait=%s\n",
	       implicit_wait_observed ? "observed" : "not-observed");
	printf("capture_implicit_fence=pass\n");
	printf("capture_frame_delivery=pass\n");
	printf("capture_damage_validation=pass\n");
	printf("capture_fence_ownership=pass\n");
	printf("capture_buffer_implicit=pass\n");

	if (create_syncobj(fd, &ready_syncobj) ||
	    create_syncobj(fd, &reuse_syncobj) ||
	    create_syncobj(fd, &second_ready_syncobj) ||
	    create_syncobj(fd, &second_reuse_syncobj))
		goto out_close;
	if (signal_syncobj_point(fd, ready_syncobj, 1))
		goto out_close;
	ioctl_ret = register_capture_buffer(fd, first_stream.stream_id,
					    first_buffer.fb_id,
		DRM_CASTKMS_CAPTURE_BUFFER_EXPLICIT_SYNC, ready_syncobj,
		reuse_syncobj, first_stream.mode_generation, &buffer_id);
	if (ioctl_ret != -EINVAL) {
		fprintf(stderr,
			"nonempty ready syncobj returned %d, expected %d\n",
			ioctl_ret, -EINVAL);
		goto out_close;
	}
	destroy_syncobj(fd, &ready_syncobj);
	if (create_syncobj(fd, &ready_syncobj))
		goto out_close;
	ioctl_ret = register_capture_buffer(fd, first_stream.stream_id,
					    first_buffer.fb_id,
		DRM_CASTKMS_CAPTURE_BUFFER_EXPLICIT_SYNC, ready_syncobj,
		ready_syncobj, first_stream.mode_generation, &buffer_id);
	if (ioctl_ret != -EINVAL) {
		fprintf(stderr,
			"shared capture syncobj returned %d, expected %d\n",
			ioctl_ret, -EINVAL);
		goto out_close;
	}
	ioctl_ret = register_capture_buffer(fd, first_stream.stream_id,
					    first_buffer.fb_id,
		DRM_CASTKMS_CAPTURE_BUFFER_EXPLICIT_SYNC, ready_syncobj,
		reuse_syncobj, first_stream.mode_generation, &buffer_id);
	if (ioctl_ret || !buffer_id) {
		errno = ioctl_ret ? -ioctl_ret : EPROTO;
		perror("register explicit capture buffer");
		goto out_close;
	}
	ioctl_ret = register_capture_buffer(fd, first_stream.stream_id,
					    second_buffer.fb_id,
		DRM_CASTKMS_CAPTURE_BUFFER_EXPLICIT_SYNC, ready_syncobj,
		second_reuse_syncobj, first_stream.mode_generation,
		&second_buffer_id);
	if (ioctl_ret != -EINVAL) {
		fprintf(stderr,
			"reused ready syncobj returned %d, expected %d\n",
			ioctl_ret, -EINVAL);
		goto out_close;
	}
	ioctl_ret = register_capture_buffer(fd, first_stream.stream_id,
					    second_buffer.fb_id,
		DRM_CASTKMS_CAPTURE_BUFFER_EXPLICIT_SYNC, second_ready_syncobj,
		reuse_syncobj, first_stream.mode_generation, &second_buffer_id);
	if (ioctl_ret != -EINVAL) {
		fprintf(stderr,
			"reused reuse syncobj returned %d, expected %d\n",
			ioctl_ret, -EINVAL);
		goto out_close;
	}
	ioctl_ret = register_capture_buffer(fd, first_stream.stream_id,
					    second_buffer.fb_id,
		DRM_CASTKMS_CAPTURE_BUFFER_EXPLICIT_SYNC, second_ready_syncobj,
		second_reuse_syncobj, first_stream.mode_generation,
		&second_buffer_id);
	if (ioctl_ret || !second_buffer_id) {
		errno = ioctl_ret ? -ioctl_ret : EPROTO;
		perror("register second explicit capture buffer");
		goto out_close;
	}
	ioctl_ret =
		queue_capture_buffer(fd, first_stream.stream_id, buffer_id,
				     DRM_CASTKMS_CAPTURE_QUEUE_EXPLICIT_SYNC,
				     first_stream.mode_generation,
				     capture_user_data + 2, 0, 0);
	if (ioctl_ret != -EINVAL) {
		fprintf(stderr,
			"zero ready point returned %d, expected %d\n",
			ioctl_ret, -EINVAL);
		goto out_close;
	}
	ioctl_ret =
		queue_capture_buffer(fd, first_stream.stream_id, buffer_id,
				     DRM_CASTKMS_CAPTURE_QUEUE_EXPLICIT_SYNC,
				     first_stream.mode_generation,
				     capture_user_data + 2, 1, 1);
	if (ioctl_ret != -EINVAL) {
		fprintf(stderr,
			"missing reuse point returned %d, expected %d\n",
			ioctl_ret, -EINVAL);
		goto out_close;
	}

	if (sync_dmabuf_cpu_access(dmabuf_fd,
				   DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE))
		goto out_close;
	memset(first_buffer.map, 0x55, first_buffer.size);
	if (sync_dmabuf_cpu_access(dmabuf_fd,
				   DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE))
		goto out_close;
	if (sync_dmabuf_cpu_access(second_dmabuf_fd,
				   DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE))
		goto out_close;
	memset(second_buffer.map, 0x66, second_buffer.size);
	if (sync_dmabuf_cpu_access(second_dmabuf_fd,
				   DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE))
		goto out_close;

	first_sequence = capture_event.sequence;
	ioctl_ret =
		queue_capture_buffer(fd, first_stream.stream_id, buffer_id,
				     DRM_CASTKMS_CAPTURE_QUEUE_EXPLICIT_SYNC,
				     first_stream.mode_generation,
				     capture_user_data + 2, 1, 0);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("queue first-use explicit capture buffer");
		goto out_close;
	}
	if (syncobj_point_is_available(fd, ready_syncobj, 1) ||
	    read_capture_event(fd, &capture_event) ||
		    wait_for_signaled_syncobj_point(fd, ready_syncobj, 1) ||
	    validate_capture_event(&capture_event, first_stream.stream_id,
				   buffer_id, capture_user_data + 2,
				   first_stream.mode_generation, width, height,
				   first_sequence, false))
		goto out_close;
	explicit_sequence = capture_event.sequence;
	if (sync_dmabuf_cpu_access(dmabuf_fd,
				   DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ))
		goto out_close;
	pixel_changed =
		*(const uint32_t *)first_buffer.map != UINT32_C(0x55555555);
	if (sync_dmabuf_cpu_access(dmabuf_fd,
				   DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ))
		goto out_close;
	if (!pixel_changed) {
		fprintf(stderr, "explicit capture did not update its destination\n");
		goto out_close;
	}
	if (sync_dmabuf_cpu_access(dmabuf_fd,
				   DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE))
		goto out_close;
	memset(first_buffer.map, 0x55, first_buffer.size);
	if (sync_dmabuf_cpu_access(dmabuf_fd,
				   DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE))
		goto out_close;
	if (sync_dmabuf_cpu_access(second_dmabuf_fd,
				   DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE))
		goto out_close;
	memset(second_buffer.map, 0x66, second_buffer.size);
	if (sync_dmabuf_cpu_access(second_dmabuf_fd,
				   DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE))
		goto out_close;

	ioctl_ret =
		queue_capture_buffer(fd, first_stream.stream_id, second_buffer_id,
				     DRM_CASTKMS_CAPTURE_QUEUE_EXPLICIT_SYNC,
				     first_stream.mode_generation,
				     capture_user_data + 3, 1, 0);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("queue explicit dependency source");
		goto out_close;
	}
	if (inspect_syncobj_point(fd, second_ready_syncobj, 1,
				  &capture_fence_pending) ||
	    transfer_syncobj_point(fd, second_ready_syncobj, 1,
				   reuse_syncobj, 1))
		goto out_close;
	ioctl_ret = queue_capture_when_available(fd, first_stream.stream_id,
						 buffer_id,
					 DRM_CASTKMS_CAPTURE_QUEUE_EXPLICIT_SYNC,
					 first_stream.mode_generation,
					 capture_user_data + 4, 2, 1);
	if (ioctl_ret) {
		fprintf(stderr,
			"queue explicit dependent buffer returned %d\n",
			ioctl_ret);
		goto out_close;
	}
	if (inspect_syncobj_point(fd, second_ready_syncobj, 1,
				  &capture_fence_pending) ||
	    inspect_syncobj_point(fd, ready_syncobj, 2,
				  &dependent_fence_pending))
		goto out_close;
	explicit_wait_observed = capture_fence_pending;
	if (read_capture_event(fd, &capture_event) ||
	    wait_for_signaled_syncobj_point(fd, second_ready_syncobj, 1) ||
	    validate_capture_event(&capture_event, first_stream.stream_id,
				   second_buffer_id, capture_user_data + 3,
				   first_stream.mode_generation, width, height,
				   explicit_sequence, false))
		goto out_close;
	explicit_sequence = capture_event.sequence;
	if (read_capture_event(fd, &capture_event) ||
	    wait_for_signaled_syncobj_point(fd, ready_syncobj, 2) ||
	    validate_capture_event(&capture_event, first_stream.stream_id,
				   buffer_id, capture_user_data + 4,
				   first_stream.mode_generation, width, height,
				   explicit_sequence, true))
		goto out_close;
	if (sync_dmabuf_cpu_access(second_dmabuf_fd,
				   DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ))
		goto out_close;
	pixel_changed =
		*(const uint32_t *)second_buffer.map != UINT32_C(0x66666666);
	if (sync_dmabuf_cpu_access(second_dmabuf_fd,
				   DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ))
		goto out_close;
	if (!pixel_changed) {
		fprintf(stderr,
			"explicit dependency source did not update its destination\n");
		goto out_close;
	}
	if (sync_dmabuf_cpu_access(dmabuf_fd,
				   DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ))
		goto out_close;
	pixel_changed =
		*(const uint32_t *)first_buffer.map != UINT32_C(0x55555555);
	if (sync_dmabuf_cpu_access(dmabuf_fd,
				   DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ))
		goto out_close;
	if (!pixel_changed) {
		fprintf(stderr,
			"explicit dependent capture did not update its destination\n");
		goto out_close;
	}

	ioctl_ret =
		queue_capture_buffer(fd, first_stream.stream_id, buffer_id,
				     DRM_CASTKMS_CAPTURE_QUEUE_EXPLICIT_SYNC,
				     first_stream.mode_generation,
				     capture_user_data + 5, 2, 2);
	if (ioctl_ret != -EINVAL) {
		fprintf(stderr,
			"repeated ready point returned %d, expected %d\n",
			ioctl_ret, -EINVAL);
		goto out_close;
	}
	ioctl_ret =
		queue_capture_buffer(fd, first_stream.stream_id, buffer_id,
				     DRM_CASTKMS_CAPTURE_QUEUE_EXPLICIT_SYNC,
				     first_stream.mode_generation,
				     capture_user_data + 5, 3, 1);
	if (ioctl_ret != -EINVAL) {
		fprintf(stderr,
			"repeated reuse point returned %d, expected %d\n",
			ioctl_ret, -EINVAL);
		goto out_close;
	}
	printf("capture_explicit_reuse_dependency=pass\n");
	printf("capture_explicit_reuse_wait=%s\n",
	       explicit_wait_observed ? "observed" : "not-observed");
	printf("capture_explicit_timeline=pass\n");

	ioctl_ret = unregister_capture_buffer(fd, first_stream.stream_id,
					      buffer_id);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("unregister explicit capture buffer");
		goto out_close;
	}
	ioctl_ret = unregister_capture_buffer(fd, first_stream.stream_id,
					      second_buffer_id);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("unregister second explicit capture buffer");
		goto out_close;
	}
	destroy_syncobj(fd, &second_reuse_syncobj);
	destroy_syncobj(fd, &second_ready_syncobj);
	destroy_syncobj(fd, &reuse_syncobj);
	destroy_syncobj(fd, &ready_syncobj);
	printf("capture_buffer_explicit=pass\n");

	ioctl_ret = register_capture_buffer(fd, first_stream.stream_id,
					    second_buffer.fb_id,
		DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC, 0, 0,
		first_stream.mode_generation, &second_buffer_id);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("register dependency source for stop cleanup");
		goto out_close;
	}
	ioctl_ret =
		queue_capture_buffer(fd, first_stream.stream_id, second_buffer_id,
				     DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC,
				     first_stream.mode_generation,
				     capture_user_data + 5, 0, 0);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("queue dependency source for stop cleanup");
		goto out_close;
	}
	second_fence_fd = export_write_fence(second_dmabuf_fd);
	if (second_fence_fd < 0 ||
	    import_read_fence(dmabuf_fd, second_fence_fd))
		goto out_close;
	ioctl_ret = register_capture_buffer(fd, first_stream.stream_id,
					    first_buffer.fb_id,
		DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC, 0, 0,
		first_stream.mode_generation, &buffer_id);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("register cancellation target for stop cleanup");
		goto out_close;
	}
	ioctl_ret = queue_capture_when_available(fd, first_stream.stream_id,
		buffer_id, DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC,
		first_stream.mode_generation, capture_user_data + 6, 0, 0);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("queue cancellation target for stop cleanup");
		goto out_close;
	}
	capture_fence_fd = export_write_fence(dmabuf_fd);
	if (capture_fence_fd < 0)
		goto out_close;
	ioctl_ret = stop_capture(fd, first_stream.stream_id);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("stop first capture stream");
		goto out_close;
	}
	if (wait_for_capture_fence(second_fence_fd) ||
	    wait_for_capture_fence_error(capture_fence_fd, -ECANCELED))
		goto out_close;
	close(second_fence_fd);
	second_fence_fd = -1;
	close(capture_fence_fd);
	capture_fence_fd = -1;
	ioctl_ret = unregister_capture_buffer(fd, first_stream.stream_id,
					      buffer_id);
	if (ioctl_ret != -ENOENT) {
		fprintf(stderr,
			"unregister after stream stop returned %d, expected %d\n",
			ioctl_ret, -ENOENT);
		goto out_close;
	}
	destroy_test_framebuffer(fd, &wrong_size_buffer);
	destroy_test_framebuffer(fd, &second_buffer);
	destroy_test_framebuffer(fd, &first_buffer);
	printf("capture_stop_cancellation=pass\n");
	printf("capture_buffer_stop_cleanup=pass\n");

	{
		uint8_t observed[512];
		uint8_t edid[TEST_EDID_BLOCK];
		uint32_t connector_id;
		uint32_t connection;
		uint32_t observed_size;

		if (fill_named_edid(edid, "CastKMS Test")) {
			fprintf(stderr, "failed to rebuild stop-check EDID\n");
			goto out_close;
		}
		if (find_display_connector(fd, crtc_id, &connector_id) ||
		    read_connector_edid(fd, connector_id, observed,
					sizeof(observed), &observed_size) ||
		    observed_size != sizeof(edid) ||
		    memcmp(observed, edid, sizeof(edid))) {
			fprintf(stderr,
				"output EDID did not remain after stream stop\n");
			goto out_close;
		}

		ioctl_ret = start_capture(competitor_fd, crtc_id, &second_stream);
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("start capture stream after stop");
			goto out_close;
		}
		if (second_stream.mode_generation != first_stream.mode_generation) {
			fprintf(stderr,
				"mode generation changed without a modeset\n");
			goto out_close;
		}
		printf("capture_stream_stop=pass\n");

		ioctl_ret = detach_monitor(fd, connector_id);
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("detach monitor after stream stop");
			goto out_close;
		}
		if (read_connector_connection(fd, connector_id, &connection) ||
		    connection != DRM_MODE_DISCONNECTED ||
		    read_connector_edid(fd, connector_id, observed,
					sizeof(observed), &observed_size) ||
		    observed_size) {
			fprintf(stderr,
				"monitor remained after explicit detach\n");
			goto out_close;
		}
		ioctl_ret = detach_monitor(fd, connector_id);
		if (ioctl_ret != -ENOTCONN) {
			fprintf(stderr,
				"detach of idle connector returned %d, expected %d\n",
				ioctl_ret, -ENOTCONN);
			goto out_close;
		}
	}

	{
		uint8_t edid[TEST_EDID_BLOCK];
		uint32_t connector_id;

		if (fill_named_edid(edid, "CastKMS File")) {
			fprintf(stderr, "failed to build file-close EDID\n");
			goto out_close;
		}
		if (find_display_connector(competitor_fd, crtc_id,
					  &connector_id)) {
			fprintf(stderr,
				"failed to find connector for file-close attach\n");
			goto out_close;
		}
		ioctl_ret = attach_monitor(competitor_fd, connector_id, edid,
					   sizeof(edid));
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("attach monitor for file-close cleanup");
			goto out_close;
		}
		if (wait_crtc_size(competitor_fd, crtc_id, &width, &height))
			goto out_close;
		ioctl_ret = stop_capture(competitor_fd, second_stream.stream_id);
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("stop competitor capture before remode");
			goto out_close;
		}
		ioctl_ret = start_capture(competitor_fd, crtc_id, &second_stream);
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("restart competitor capture after attach");
			goto out_close;
		}
		ioctl_ret = set_output_edid(competitor_fd,
					    second_stream.stream_id, edid,
					    sizeof(edid));
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("set output EDID for file-close cleanup");
			goto out_close;
		}
	}

	if (create_test_framebuffer(competitor_fd, width, height,
				    &competitor_buffer) ||
	    create_syncobj(competitor_fd, &competitor_ready_syncobj) ||
	    create_syncobj(competitor_fd, &competitor_reuse_syncobj))
		goto out_close;
	ioctl_ret = register_capture_buffer(competitor_fd,
					    second_stream.stream_id,
					    competitor_buffer.fb_id,
		DRM_CASTKMS_CAPTURE_BUFFER_EXPLICIT_SYNC,
		competitor_ready_syncobj, competitor_reuse_syncobj,
		second_stream.mode_generation, &buffer_id);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("register capture buffer for file-close cleanup");
		goto out_close;
	}
	ioctl_ret = queue_capture_buffer(competitor_fd,
					 second_stream.stream_id, buffer_id,
					 DRM_CASTKMS_CAPTURE_QUEUE_EXPLICIT_SYNC,
					 second_stream.mode_generation,
					 capture_user_data + 6, 1, 0);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("queue capture buffer for file-close cleanup");
		goto out_close;
	}

	unmap_test_framebuffer(&competitor_buffer);
	close(competitor_fd);
	competitor_fd = -1;
	competitor_buffer = (struct test_framebuffer) {};
	competitor_ready_syncobj = 0;
	competitor_reuse_syncobj = 0;
	verifier_fd = open_capture_device(argv[1], false);
	if (verifier_fd < 0)
		goto out_close;
	ioctl_ret = start_capture(verifier_fd, crtc_id, &verifier_stream);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("start capture stream after file close");
		goto out_close;
	}
	{
		uint8_t observed[512];
		uint32_t connector_id;
		uint32_t observed_size;

		if (find_display_connector(verifier_fd, crtc_id,
					  &connector_id) ||
		    read_connector_edid(verifier_fd, connector_id, observed,
					sizeof(observed), &observed_size) ||
		    observed_size) {
			fprintf(stderr,
				"output EDID remained after file close\n");
			goto out_close;
		}
	}
	ioctl_ret = stop_capture(verifier_fd, verifier_stream.stream_id);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("stop verifier capture stream");
		goto out_close;
	}
	printf("capture_mode_generation=%llu\n",
	       (unsigned long long)verifier_stream.mode_generation);
	printf("capture_buffer_postclose=pass\n");
	printf("capture_buffer_registration=pass\n");
	printf("capture_stream_postclose=pass\n");
	printf("capture_stream_lifecycle=pass\n");
	ret = EXIT_SUCCESS;

out_close:
	if (second_fence_fd >= 0)
		close(second_fence_fd);
	if (capture_fence_fd >= 0)
		close(capture_fence_fd);
	if (second_dmabuf_fd >= 0)
		close(second_dmabuf_fd);
	if (dmabuf_fd >= 0)
		close(dmabuf_fd);
	if (verifier_fd >= 0)
		close(verifier_fd);
	if (competitor_fd >= 0) {
		destroy_syncobj(competitor_fd, &competitor_reuse_syncobj);
		destroy_syncobj(competitor_fd, &competitor_ready_syncobj);
		destroy_test_framebuffer(competitor_fd, &competitor_buffer);
		close(competitor_fd);
	}
	destroy_syncobj(fd, &second_reuse_syncobj);
	destroy_syncobj(fd, &second_ready_syncobj);
	destroy_syncobj(fd, &reuse_syncobj);
	destroy_syncobj(fd, &ready_syncobj);
	destroy_test_framebuffer(fd, &wrong_size_buffer);
	destroy_test_framebuffer(fd, &second_buffer);
	destroy_test_framebuffer(fd, &first_buffer);
	close(fd);
	return ret;
}
