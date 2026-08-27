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
#include <time.h>
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
static_assert(sizeof(struct drm_castkms_capture_register_buffer) == 24,
	      "capture register ABI size changed");
static_assert(sizeof(struct drm_castkms_capture_unregister_buffer) == 16,
	      "capture unregister ABI size changed");
static_assert(sizeof(struct drm_castkms_capture_queue_buffer) == 32,
	      "capture queue ABI size changed");
static_assert(sizeof(struct drm_event_castkms_capture_frame) == 64,
	      "capture event ABI size changed");
static_assert(offsetof(struct drm_event_castkms_capture_frame, reserved) == 60,
	      "capture event ABI layout changed");
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

static int register_capture_buffer(int fd, uint32_t stream_id,
				   uint32_t fb_id, uint32_t flags,
				   uint64_t mode_generation,
				   uint32_t *buffer_id)
{
	return castkms_test_capture_register_buffer(
		fd, stream_id, fb_id, flags, mode_generation, buffer_id);
}

static int unregister_capture_buffer(int fd, uint32_t stream_id,
				     uint32_t buffer_id)
{
	return castkms_test_capture_unregister_buffer(fd, stream_id, buffer_id);
}

static int queue_capture_buffer(int fd, uint32_t stream_id,
				uint32_t buffer_id, uint32_t flags,
				uint64_t mode_generation, uint64_t user_data)
{
	return castkms_test_capture_queue_buffer(
		fd, stream_id, buffer_id, flags, mode_generation, user_data);
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

static int
queue_capture_when_available(int fd, uint32_t stream_id, uint32_t buffer_id,
			     uint32_t flags, uint64_t mode_generation,
			     uint64_t user_data)
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
					   mode_generation, user_data);
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
	struct timespec now;
	int64_t deadline_ms;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return -1;
	deadline_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 +
		timeout_ms;

	for (;;) {
		uint64_t aligned_events[sizeof(*event) / sizeof(uint64_t)];
		unsigned char *events = (unsigned char *)aligned_events;
		ssize_t length;
		int64_t now_ms;
		int remaining_ms;
		int ret;

		if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
			return -1;
		now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
		if (now_ms >= deadline_ms) {
			fprintf(stderr, "timed out waiting for capture event\n");
			return -1;
		}
		remaining_ms = (int)(deadline_ms - now_ms);
		poll_fd.revents = 0;
		ret = poll(&poll_fd, 1, remaining_ms);
		if (ret < 0) {
			perror("poll capture event");
			return -1;
		}
		if (!ret || !(poll_fd.revents & POLLIN)) {
			fprintf(stderr, "timed out waiting for capture event\n");
			return -1;
		}

		length = read(fd, events, sizeof(aligned_events));
		if (length < 0) {
			perror("read capture event");
			return -1;
		}
		for (ssize_t offset = 0; offset < length;) {
			const struct drm_event *base = (const void *)(events + offset);

			if (length - offset < (ssize_t)sizeof(*base) ||
			    base->length < sizeof(*base) ||
			    (ssize_t)base->length > length - offset) {
				fprintf(stderr, "malformed DRM event stream\n");
				return -1;
			}
			if (base->type == DRM_CASTKMS_CAPTURE_EVENT_FRAME) {
				if (base->length != sizeof(*event)) {
					fprintf(stderr, "unexpected capture event size: %u\n",
						base->length);
					return -1;
				}
				memcpy(event, base, sizeof(*event));
				return 0;
			}
			if (base->type == DRM_CASTKMS_CAPTURE_EVENT_GRANT_STATE) {
				const struct drm_event_castkms_grant_state *state =
					(const void *)base;

				if (base->length != sizeof(*state) ||
				    state->state > DRM_CASTKMS_GRANT_STATE_REVOKED ||
				    state->reserved) {
					fprintf(stderr, "invalid grant-state event\n");
					return -1;
				}
				offset += base->length;
				continue;
			}
			if (base->type ==
			    DRM_CASTKMS_CAPTURE_EVENT_GRANT_REVOKED) {
				const struct drm_event_castkms_grant_revoked *revoked =
					(const void *)base;

				if (base->length != sizeof(*revoked) ||
				    revoked->status >= 0) {
					fprintf(stderr, "invalid grant-revoked event\n");
					return -1;
				}
				fprintf(stderr, "capture grant was revoked: %s\n",
					strerror(-revoked->status));
				return -1;
			}
			fprintf(stderr, "unexpected DRM event type: %#x\n",
				base->type);
			return -1;
		}
	}
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
		       uint64_t after_sequence,
		       bool may_have_dropped_frames)
{
	if (event->base.type != DRM_CASTKMS_CAPTURE_EVENT_FRAME ||
	    event->base.length != sizeof(*event) ||
	    event->user_data != user_data || event->stream_id != stream_id ||
	    event->buffer_id != buffer_id || event->status ||
	    event->flags ||
	    event->sequence <= after_sequence ||
	    ((!may_have_dropped_frames && event->dropped_frames) ||
	     (may_have_dropped_frames &&
	      event->dropped_frames > event->sequence - after_sequence - 1)) ||
	    event->timestamp_ns <= 0 ||
	    event->mode_generation != mode_generation ||
	    event->reserved) {
		fprintf(stderr, "unexpected capture completion event\n");
		return -1;
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
				   struct castkms_test_framebuffer *buffer)
{
	return castkms_test_framebuffer_create(fd, width, height, true, buffer);
}

static void destroy_test_framebuffer(
	int fd, struct castkms_test_framebuffer *buffer)
{
	castkms_test_framebuffer_destroy(fd, buffer);
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

static int wait_for_capture_fence_stop(int fence_fd)
{
	struct sync_fence_info *fences;
	struct sync_file_info info;
	struct pollfd poll_fd = {
		.fd = fence_fd,
		.events = POLLIN,
	};
	int ret;

	ret = poll(&poll_fd, 1, 2000);
	if (ret <= 0 || !(poll_fd.revents & POLLIN)) {
		fprintf(stderr, "timed out waiting for stopped capture fence\n");
		return -1;
	}
	if (read_sync_file_info(fence_fd, &info, &fences))
		return -1;

	/* STOP may race completion of an already selected source frame. */
	ret = info.status == 1 || info.status == -ECANCELED ? 0 : -1;
	for (uint32_t i = 0; i < info.num_fences; i++) {
		if (!capture_fence_name_is_valid(&fences[i]) ||
		    (fences[i].status != 1 &&
		     fences[i].status != -ECANCELED) ||
		    (fences[i].status == 1 && !fences[i].timestamp_ns))
			ret = -1;
	}
	free(fences);

	if (ret)
		fprintf(stderr, "capture source fence did not finish on stop\n");
	return ret;
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
	    query->reserved || query->max_registered_buffers < 2) {
		fprintf(stderr, "unexpected capture query result\n");
		return -1;
	}
	if (!(query->flags & DRM_CASTKMS_CAPTURE_CAP_IMPLICIT_SYNC)) {
		fprintf(stderr, "capture query lacks implicit sync support\n");
		return -1;
	}
	if (!(query->flags & DRM_CASTKMS_CAPTURE_CAP_GRANT_FD)) {
		fprintf(stderr, "capture query lacks grant-fd support\n");
		return -1;
	}

	return 0;
}

static int run_deliver_one(int inherited_fd, uint32_t crtc_id)
{
	const uint64_t user_data = 0x434153544b4d5305ULL;
	struct drm_event_castkms_capture_frame event = {};
	struct drm_castkms_capture_start stream;
	struct castkms_test_framebuffer buffer = {};
	uint32_t buffer_id = 0;
	uint32_t height;
	uint32_t width;
	bool fence_pending;
	int dmabuf_fd = -1;
	int fd;
	int fence_fd = -1;
	int ioctl_ret;
	int ret = EXIT_FAILURE;

	fd = open_capture_grant(inherited_fd,
		DRM_CASTKMS_GRANT_CAPTURE_PIXELS |
		DRM_CASTKMS_GRANT_READ_CURSOR, false);
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
		DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC,
		stream.mode_generation, &buffer_id);
	if (ioctl_ret || !buffer_id) {
		errno = ioctl_ret ? -ioctl_ret : EPROTO;
		perror("register overlap capture buffer");
		goto out_close;
	}

	ioctl_ret = queue_capture_buffer(fd, stream.stream_id, buffer_id,
					 DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC,
					 stream.mode_generation, user_data);
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
				   user_data, stream.mode_generation,
				   0, true))
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

static void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s [--grant-fd FD] [--deliver-one|--mode-generation] DRM-DEVICE CRTC-ID\n",
		program);
}

int main(int argc, char **argv)
{
	const uint64_t capture_user_data = 0x434153544b4d5304ULL;
	struct drm_event_castkms_capture_frame capture_event = {};
	struct drm_castkms_capture_format format = {};
	struct drm_castkms_capture_query_caps query = {};
	struct drm_castkms_capture_start first_stream;
	struct drm_castkms_capture_start second_stream;
	struct castkms_test_framebuffer first_buffer = {};
	struct castkms_test_framebuffer second_buffer = {};
	struct castkms_test_framebuffer wrong_size_buffer = {};
	uint32_t *extra_buffer_ids = NULL;
	uint32_t buffer_id;
	uint32_t crtc_id;
	uint32_t height;
	uint32_t second_buffer_id;
	uint32_t width;
	uint64_t first_sequence;
	bool capture_fence_pending;
	bool dependent_fence_pending;
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
	    (!strcmp(argv[argument], "--deliver-one") ||
	     !strcmp(argv[argument], "--mode-generation")))
		mode = argv[argument++];
	if (argc - argument != 2) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	device = argv[argument];
	if (parse_crtc_id(argv[argument + 1], &crtc_id))
		return EXIT_FAILURE;

	if (mode && !strcmp(mode, "--deliver-one"))
		return run_deliver_one(inherited_fd, crtc_id);
	if (mode && !strcmp(mode, "--mode-generation")) {
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
		fprintf(stderr, "ordinary-fd capture start returned %d, expected %d\n",
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
		if (wait_crtc_size(fd, crtc_id, &width, &height))
			goto out_close;
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
		DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC,
		first_stream.mode_generation + 1, &buffer_id);
	if (ioctl_ret != -ESTALE) {
		fprintf(stderr,
			"stale capture buffer registration returned %d, expected %d\n",
			ioctl_ret, -ESTALE);
		goto out_close;
	}
	ioctl_ret = register_capture_buffer(fd, first_stream.stream_id,
					    wrong_size_buffer.fb_id,
		DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC,
		first_stream.mode_generation, &buffer_id);
	if (ioctl_ret != -EINVAL) {
		fprintf(stderr,
			"wrong-size capture buffer returned %d, expected %d\n",
			ioctl_ret, -EINVAL);
		goto out_close;
	}
	printf("capture_buffer_rejections=pass\n");

	ioctl_ret = register_capture_buffer(fd, first_stream.stream_id,
					    first_buffer.fb_id,
		DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC,
		first_stream.mode_generation, &buffer_id);
	if (ioctl_ret || !buffer_id) {
		errno = ioctl_ret ? -ioctl_ret : EPROTO;
		perror("register implicit capture buffer");
		goto out_close;
	}
	ioctl_ret = register_capture_buffer(fd, first_stream.stream_id,
					    second_buffer.fb_id,
		DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC,
		first_stream.mode_generation, &second_buffer_id);
	if (ioctl_ret || !second_buffer_id) {
		errno = ioctl_ret ? -ioctl_ret : EPROTO;
		perror("register second implicit capture buffer");
		goto out_close;
	}
	if (query.max_registered_buffers > 2) {
		extra_buffer_ids = calloc(query.max_registered_buffers - 2,
					  sizeof(*extra_buffer_ids));
		if (!extra_buffer_ids) {
			perror("allocate capture buffer identifiers");
			goto out_close;
		}
	}
	for (uint32_t i = 2; i < query.max_registered_buffers; i++) {
		ioctl_ret = register_capture_buffer(fd, first_stream.stream_id,
			first_buffer.fb_id,
			DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC,
			first_stream.mode_generation, &extra_buffer_ids[i - 2]);
		if (ioctl_ret || !extra_buffer_ids[i - 2]) {
			errno = ioctl_ret ? -ioctl_ret : EPROTO;
			perror("register buffer up to advertised limit");
			goto out_close;
		}
	}
	{
		uint32_t rejected_id = 0;

		ioctl_ret = register_capture_buffer(fd, first_stream.stream_id,
			first_buffer.fb_id,
			DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC,
			first_stream.mode_generation, &rejected_id);
		if (ioctl_ret != -ENOSPC) {
			fprintf(stderr,
				"buffer past advertised limit returned %d, expected %d\n",
				ioctl_ret, -ENOSPC);
			goto out_close;
		}
	}
	for (uint32_t i = 2; i < query.max_registered_buffers; i++) {
		ioctl_ret = unregister_capture_buffer(fd, first_stream.stream_id,
						      extra_buffer_ids[i - 2]);
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("unregister buffer-limit probe");
			goto out_close;
		}
		extra_buffer_ids[i - 2] = 0;
	}
	free(extra_buffer_ids);
	extra_buffer_ids = NULL;
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
				     capture_user_data);
	if (ioctl_ret != -ESTALE) {
		fprintf(stderr,
			"stale capture queue returned %d, expected %d\n",
			ioctl_ret, -ESTALE);
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
				     capture_user_data);
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
					 capture_user_data + 1);
	if (ioctl_ret) {
		fprintf(stderr,
			"queue dependent capture buffer returned %d\n",
			ioctl_ret);
		goto out_close;
	}
	ioctl_ret = queue_capture_buffer(fd, first_stream.stream_id, buffer_id,
					 DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC,
					 first_stream.mode_generation,
					 capture_user_data + 2);
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
				   first_stream.mode_generation,
				   0, false))
		goto out_close;
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
				   first_stream.mode_generation,
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
	printf("capture_fence_ownership=pass\n");
	printf("capture_buffer_implicit=pass\n");

	ioctl_ret = register_capture_buffer(fd, first_stream.stream_id,
					    second_buffer.fb_id,
		DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC,
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
				     capture_user_data + 5);
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
		DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC,
		first_stream.mode_generation, &buffer_id);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("register cancellation target for stop cleanup");
		goto out_close;
	}
	ioctl_ret = queue_capture_when_available(fd, first_stream.stream_id,
		buffer_id, DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC,
		first_stream.mode_generation, capture_user_data + 6);
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
	if (wait_for_capture_fence_stop(second_fence_fd) ||
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
		ioctl_ret = start_capture(fd, crtc_id, &second_stream);
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
		ioctl_ret = stop_capture(fd, second_stream.stream_id);
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("stop restarted capture stream");
			goto out_close;
		}
	}
	printf("capture_mode_generation=%llu\n",
	       (unsigned long long)second_stream.mode_generation);
	printf("capture_buffer_registration=pass\n");
	printf("capture_stream_lifecycle=pass\n");
	ret = EXIT_SUCCESS;

out_close:
	free(extra_buffer_ids);
	if (second_fence_fd >= 0)
		close(second_fence_fd);
	if (capture_fence_fd >= 0)
		close(capture_fence_fd);
	if (second_dmabuf_fd >= 0)
		close(second_dmabuf_fd);
	if (dmabuf_fd >= 0)
		close(dmabuf_fd);
	if (competitor_fd >= 0)
		close(competitor_fd);
	destroy_test_framebuffer(fd, &wrong_size_buffer);
	destroy_test_framebuffer(fd, &second_buffer);
	destroy_test_framebuffer(fd, &first_buffer);
	close(fd);
	return ret;
}
