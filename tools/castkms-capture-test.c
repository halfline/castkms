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

#include "castkms-test-drm.h"
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
static_assert(sizeof(struct drm_castkms_capture_read_cursor_bitmap) == 40,
	      "capture read-cursor-bitmap ABI size changed");
static_assert(sizeof(struct drm_castkms_get_grant) == 32,
	      "get-grant ABI size changed");
static_assert(sizeof(struct drm_event_castkms_grant_revoked) == 24,
	      "grant-revoked event ABI size changed");
static_assert(sizeof(struct drm_event_castkms_grant_state) == 32,
	      "grant-state event ABI size changed");

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
	    grant.reserved) {
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
				   uint32_t ready_syncobj_handle,
				   uint32_t reuse_syncobj_handle,
				   uint64_t mode_generation,
				   uint32_t *buffer_id)
{
	return castkms_test_capture_register_buffer(
		fd, stream_id, fb_id, flags, ready_syncobj_handle,
		reuse_syncobj_handle, mode_generation, buffer_id);
}

static int unregister_capture_buffer(int fd, uint32_t stream_id,
				     uint32_t buffer_id)
{
	return castkms_test_capture_unregister_buffer(fd, stream_id, buffer_id);
}

static int queue_capture_buffer(int fd, uint32_t stream_id,
				uint32_t buffer_id, uint32_t flags,
				uint64_t mode_generation, uint64_t user_data,
				uint64_t ready_point, uint64_t reuse_point)
{
	return castkms_test_capture_queue_buffer(
		fd, stream_id, buffer_id, flags, mode_generation, user_data,
		ready_point, reuse_point);
}

#define TEST_EDID_BLOCK CASTKMS_EDID_BLOCK

static int fill_named_edid(uint8_t edid[TEST_EDID_BLOCK], const char *name)
{
	return castkms_fill_named_edid(edid, name);
}

static int set_output_edid(int fd, uint32_t connector_id, const void *edid,
			   uint32_t size)
{
	struct drm_castkms_capture_set_output_edid args = {
		.connector_id = connector_id,
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
	struct drm_mode_get_connector connector = {
		.connector_id = connector_id,
	};

	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) < 0)
		return -1;

	*connection = connector.connection;
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

static int read_connector_edid(int fd, uint32_t connector_id, uint8_t *edid,
			       uint32_t capacity, uint32_t *size)
{
	struct drm_mode_get_connector conn = {
		.connector_id = connector_id,
	};
	uint32_t *props = NULL;
	uint64_t *values = NULL;
	uint32_t prop_capacity;
	uint32_t i;
	int ret = -1;

	*size = 0;
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
		return -1;
	if (!conn.count_props)
		return 0;
	prop_capacity = conn.count_props;
	props = calloc(prop_capacity, sizeof(*props));
	values = calloc(prop_capacity, sizeof(*values));
	if (!props || !values)
		goto out;
	conn.count_modes = 0;
	conn.count_encoders = 0;
	conn.props_ptr = (uint64_t)(uintptr_t)props;
	conn.prop_values_ptr = (uint64_t)(uintptr_t)values;
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0 ||
	    conn.count_props > prop_capacity)
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
		if (ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) < 0 ||
		    blob.length > capacity)
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
validate_capture_cursor(const struct drm_event_castkms_capture_frame *event)
{
	const uint32_t known_flags = DRM_CASTKMS_CURSOR_VISIBLE |
				     DRM_CASTKMS_CURSOR_IMAGE_CHANGED;
	bool visible = event->cursor_flags & DRM_CASTKMS_CURSOR_VISIBLE;

	if (event->cursor_flags & ~known_flags ||
	    (!event->cursor_serial &&
	     (event->cursor_flags || event->cursor_x || event->cursor_y ||
	      event->cursor_hotspot_x || event->cursor_hotspot_y ||
	      event->cursor_width || event->cursor_height)) ||
	    (visible && (!event->cursor_width || !event->cursor_height)) ||
	    (!visible && (event->cursor_width || event->cursor_height))) {
		fprintf(stderr, "invalid cursor metadata in capture event\n");
		return -1;
	}

	return 0;
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

	return validate_capture_cursor(event);
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

static int set_connector_crtc(int fd, uint32_t connector_id,
			      uint32_t crtc_id,
			      uint32_t *out_width, uint32_t *out_height)
{
	struct drm_mode_get_connector conn = {
		.connector_id = connector_id,
	};
	struct drm_mode_modeinfo *modes = NULL;
	struct drm_mode_create_dumb dumb = {};
	struct drm_mode_fb_cmd2 fb = {};
	struct drm_mode_crtc set = {};
	struct drm_mode_destroy_dumb destroy;
	int ret = -1;

	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
		perror("GETCONNECTOR count");
		return -1;
	}
	if (!conn.count_modes) {
		fprintf(stderr, "connector has no modes after attach\n");
		return -1;
	}

	modes = calloc(conn.count_modes, sizeof(*modes));
	if (!modes)
		return -1;
	conn = (struct drm_mode_get_connector){
		.connector_id = connector_id,
		.count_modes = conn.count_modes,
		.modes_ptr = (uint64_t)(uintptr_t)modes,
	};
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
		perror("GETCONNECTOR modes");
		goto out_modes;
	}

	dumb.width = modes[0].hdisplay;
	dumb.height = modes[0].vdisplay;
	dumb.bpp = 32;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) < 0) {
		perror("create modeset dumb buffer");
		goto out_modes;
	}

	fb.width = modes[0].hdisplay;
	fb.height = modes[0].vdisplay;
	fb.pixel_format = DRM_FORMAT_XRGB8888;
	fb.handles[0] = dumb.handle;
	fb.pitches[0] = dumb.pitch;
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0) {
		perror("add modeset fb");
		goto out_dumb;
	}

	set.crtc_id = crtc_id;
	set.fb_id = fb.fb_id;
	set.set_connectors_ptr = (uint64_t)(uintptr_t)&connector_id;
	set.count_connectors = 1;
	set.mode = modes[0];
	set.mode_valid = 1;
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
		perror("SETCRTC");
		ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb.fb_id);
		goto out_dumb;
	}

	*out_width = modes[0].hdisplay;
	*out_height = modes[0].vdisplay;
	ret = 0;

out_dumb:
	if (ret) {
		destroy = (struct drm_mode_destroy_dumb){ .handle = dumb.handle };
		ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	}
out_modes:
	free(modes);
	return ret;
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
	if (!(query->flags & DRM_CASTKMS_CAPTURE_CAP_SYNCOBJ_TIMELINE)) {
		fprintf(stderr, "capture query lacks timeline syncobj support\n");
		return -1;
	}
	if (!(query->flags & DRM_CASTKMS_CAPTURE_CAP_GRANT_FD)) {
		fprintf(stderr, "capture query lacks grant-fd support\n");
		return -1;
	}
	if (!(query->flags & DRM_CASTKMS_CAPTURE_CAP_GRANT_CONTROL_FD)) {
		fprintf(stderr,
			"capture query lacks grant-control-fd support\n");
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

static int run_cursor_test(const char *device, int inherited_fd,
			   uint32_t crtc_id)
{
	const uint32_t cursor_w = 64, cursor_h = 64;
	const int32_t cursor_x = 10, cursor_y = 20;
	const int32_t hot_x = 5, hot_y = 7;
	const uint32_t cursor_fill = 0xFFFF0000;
	const uint64_t user_data = 0x4355525330520001ULL;

	struct drm_mode_create_dumb cursor_dumb = {
		.width = cursor_w, .height = cursor_h, .bpp = 32,
	};
	struct drm_mode_map_dumb cursor_map_req = {};
	void *cursor_pixels = NULL;
	uint32_t connector_id = 0;
	bool attached = false;

	struct drm_castkms_capture_start stream = {};
	struct castkms_test_framebuffer capture_buf = {};
	struct castkms_test_framebuffer second_capture_buf = {};
	struct drm_event_castkms_capture_frame event = {};
	uint32_t buffer_id = 0;
	uint32_t second_buffer_id = 0;
	uint32_t width, height;
	uint8_t edid[TEST_EDID_BLOCK];
	uint32_t first_serial;
	uint32_t last_visible_serial;
	uint64_t sequence = 0;
	int ioctl_ret;
	int grant_fd = -1;
	int kms_fd = -1;
	int ret = EXIT_FAILURE;

	grant_fd = open_capture_grant(inherited_fd,
		DRM_CASTKMS_GRANT_CAPTURE_PIXELS |
		DRM_CASTKMS_GRANT_MANAGE_ATTACHMENT |
		DRM_CASTKMS_GRANT_UPDATE_EDID |
		DRM_CASTKMS_GRANT_READ_CURSOR, false);
	if (grant_fd < 0)
		return EXIT_FAILURE;
	kms_fd = open(device, O_RDWR | O_CLOEXEC);
	if (kms_fd < 0) {
		perror("open DRM device");
		goto out;
	}
	if (castkms_test_check_driver_name(kms_fd))
		goto out;

	if (find_display_connector(grant_fd, crtc_id, &connector_id))
		goto out;
	if (fill_named_edid(edid, "CursorTest"))
		goto out;
	ioctl_ret = attach_monitor(grant_fd, connector_id, edid, sizeof(edid));
	if (ioctl_ret && ioctl_ret != -EBUSY) {
		errno = -ioctl_ret;
		perror("ATTACH_MONITOR");
		goto out;
	}
	attached = ioctl_ret == 0;
	if (set_connector_crtc(kms_fd, connector_id, crtc_id, &width, &height))
		goto out;

	if (ioctl(kms_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cursor_dumb) < 0) {
		perror("create cursor dumb buffer");
		goto out;
	}
	cursor_map_req.handle = cursor_dumb.handle;
	if (ioctl(kms_fd, DRM_IOCTL_MODE_MAP_DUMB, &cursor_map_req) < 0) {
		perror("map cursor dumb buffer");
		goto out;
	}
	cursor_pixels = mmap(NULL, cursor_dumb.size, PROT_READ | PROT_WRITE,
			     MAP_SHARED, kms_fd, cursor_map_req.offset);
	if (cursor_pixels == MAP_FAILED) {
		cursor_pixels = NULL;
		perror("mmap cursor buffer");
		goto out;
	}
	for (uint32_t i = 0; i < cursor_w * cursor_h; i++)
		((uint32_t *)cursor_pixels)[i] = cursor_fill;

	{
		struct drm_mode_cursor2 cur = {
			.flags = DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE,
			.crtc_id = crtc_id,
			.x = cursor_x, .y = cursor_y,
			.width = cursor_w, .height = cursor_h,
			.handle = cursor_dumb.handle,
			.hot_x = hot_x, .hot_y = hot_y,
		};
		if (ioctl(kms_fd, DRM_IOCTL_MODE_CURSOR2, &cur) < 0) {
			perror("set cursor");
			goto out;
		}
	}

	{
		struct drm_castkms_capture_start start = {
			.crtc_id = crtc_id,
			.flags = DRM_CASTKMS_CAPTURE_START_EXCLUSIVE |
				 DRM_CASTKMS_CAPTURE_START_EXCLUDE_CURSOR,
		};
		if (ioctl(grant_fd, DRM_IOCTL_CASTKMS_CAPTURE_START, &start) < 0) {
			perror("start capture");
			goto out;
		}
		stream = start;
	}

	if (create_test_framebuffer(grant_fd, width, height, &capture_buf) ||
	    create_test_framebuffer(grant_fd, width, height,
				    &second_capture_buf))
		goto out;
	ioctl_ret = register_capture_buffer(grant_fd, stream.stream_id,
					    capture_buf.fb_id,
		DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC, 0, 0,
		stream.mode_generation, &buffer_id);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("register capture buffer");
		goto out;
	}
	ioctl_ret = register_capture_buffer(grant_fd, stream.stream_id,
					    second_capture_buf.fb_id,
		DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC, 0, 0,
		stream.mode_generation, &second_buffer_id);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("register second capture buffer");
		goto out;
	}

	/* First capture: cursor visible, IMAGE_CHANGED expected */
	ioctl_ret = queue_capture_buffer(grant_fd, stream.stream_id, buffer_id,
					 DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC,
					 stream.mode_generation, user_data, 0, 0);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("queue first capture");
		goto out;
	}
	if (read_capture_event(grant_fd, &event) ||
	    validate_capture_event(&event, stream.stream_id, buffer_id,
				   user_data, stream.mode_generation,
				   width, height, sequence, false))
		goto out;
	sequence = event.sequence;
	if (!event.cursor_serial) {
		fprintf(stderr, "cursor_serial is zero with active cursor\n");
		goto out;
	}
	if (!(event.cursor_flags & DRM_CASTKMS_CURSOR_VISIBLE)) {
		fprintf(stderr, "CURSOR_VISIBLE not set with active cursor\n");
		goto out;
	}
	if (!(event.cursor_flags & DRM_CASTKMS_CURSOR_IMAGE_CHANGED)) {
		fprintf(stderr,
			"CURSOR_IMAGE_CHANGED not set on first capture\n");
		goto out;
	}
	if (event.cursor_x != cursor_x || event.cursor_y != cursor_y) {
		fprintf(stderr, "cursor position: (%d,%d), expected (%d,%d)\n",
			event.cursor_x, event.cursor_y, cursor_x, cursor_y);
		goto out;
	}
	if (event.cursor_hotspot_x != (uint32_t)hot_x ||
	    event.cursor_hotspot_y != (uint32_t)hot_y) {
		fprintf(stderr, "cursor hotspot: (%u,%u), expected (%d,%d)\n",
			event.cursor_hotspot_x, event.cursor_hotspot_y,
			hot_x, hot_y);
		goto out;
	}
	if (event.cursor_width != cursor_w ||
	    event.cursor_height != cursor_h) {
		fprintf(stderr, "cursor size: %ux%u, expected %ux%u\n",
			event.cursor_width, event.cursor_height,
			cursor_w, cursor_h);
		goto out;
	}
	first_serial = event.cursor_serial;
	printf("cursor_metadata=pass\n");

	/* READ_CURSOR_BITMAP: probe then fetch */
	{
		struct drm_castkms_capture_read_cursor_bitmap bmp = {
			.stream_id = stream.stream_id,
			.buffer_id = buffer_id,
		};
		uint32_t required_size;
		void *bitmap;

		if (ioctl(grant_fd, DRM_IOCTL_CASTKMS_CAPTURE_READ_CURSOR_BITMAP,
			  &bmp) < 0) {
			perror("probe cursor bitmap size");
			goto out;
		}
		if (bmp.format != DRM_FORMAT_ARGB8888) {
			fprintf(stderr, "cursor bitmap format: %#x\n",
				bmp.format);
			goto out;
		}
		if (bmp.width != cursor_w || bmp.height != cursor_h) {
			fprintf(stderr, "cursor bitmap size: %ux%u\n",
				bmp.width, bmp.height);
			goto out;
		}
		if (bmp.stride != cursor_w * sizeof(uint32_t) ||
		    bmp.bitmap_size != bmp.stride * cursor_h) {
			fprintf(stderr,
				"cursor bitmap layout: stride=%u size=%u\n",
				bmp.stride, bmp.bitmap_size);
			goto out;
		}

		required_size = bmp.bitmap_size;
		bitmap = malloc(required_size);
		if (!bitmap)
			goto out;
		bmp.bitmap_ptr = (uint64_t)(uintptr_t)bitmap;
		bmp.bitmap_size = required_size - 1;
		errno = 0;
		if (ioctl(grant_fd,
			  DRM_IOCTL_CASTKMS_CAPTURE_READ_CURSOR_BITMAP,
			  &bmp) == 0 || errno != ENOSPC ||
		    bmp.bitmap_size != required_size) {
			fprintf(stderr,
				"short cursor bitmap read: errno=%s size=%u\n",
				strerror(errno), bmp.bitmap_size);
			free(bitmap);
			goto out;
		}
		bmp.bitmap_size = required_size;
		if (ioctl(grant_fd, DRM_IOCTL_CASTKMS_CAPTURE_READ_CURSOR_BITMAP,
			  &bmp) < 0) {
			perror("read cursor bitmap");
			free(bitmap);
			goto out;
		}
		if (((uint32_t *)bitmap)[0] != cursor_fill) {
			fprintf(stderr,
				"cursor bitmap pixel[0]: %#x, expected %#x\n",
				((uint32_t *)bitmap)[0], cursor_fill);
			free(bitmap);
			goto out;
		}
		free(bitmap);
	}
	printf("cursor_bitmap=pass\n");

	/* Second capture: same cursor, IMAGE_CHANGED must NOT be set */
	ioctl_ret = queue_capture_when_available(grant_fd, stream.stream_id,
		second_buffer_id, DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC,
		stream.mode_generation, user_data + 1, 0, 0);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("queue second capture");
		goto out;
	}
	if (read_capture_event(grant_fd, &event) ||
	    validate_capture_event(&event, stream.stream_id, second_buffer_id,
				   user_data + 1, stream.mode_generation,
				   width, height, sequence, false))
		goto out;
	sequence = event.sequence;
	if (event.cursor_serial != first_serial) {
		fprintf(stderr,
			"cursor_serial changed without image change: %u vs %u\n",
			event.cursor_serial, first_serial);
		goto out;
	}
	if (event.cursor_flags & DRM_CASTKMS_CURSOR_IMAGE_CHANGED) {
		fprintf(stderr,
			"IMAGE_CHANGED set without cursor change\n");
		goto out;
	}
	printf("cursor_stream_image_state=pass\n");
	printf("cursor_no_change=pass\n");

	/* Move cursor: position only, serial must stay the same */
	{
		struct drm_mode_cursor2 cur = {
			.flags = DRM_MODE_CURSOR_MOVE,
			.crtc_id = crtc_id,
			.x = cursor_x + 50, .y = cursor_y + 50,
		};
		if (ioctl(kms_fd, DRM_IOCTL_MODE_CURSOR2, &cur) < 0) {
			perror("move cursor");
			goto out;
		}
	}
	ioctl_ret = queue_capture_when_available(grant_fd, stream.stream_id,
		buffer_id, DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC,
		stream.mode_generation, user_data + 2, 0, 0);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("queue capture after move");
		goto out;
	}
	if (read_capture_event(grant_fd, &event) ||
	    validate_capture_event(&event, stream.stream_id, buffer_id,
				   user_data + 2, stream.mode_generation,
				   width, height, sequence, false))
		goto out;
	sequence = event.sequence;
	if (event.cursor_x != cursor_x + 50 ||
	    event.cursor_y != cursor_y + 50) {
		fprintf(stderr, "cursor position after move: (%d,%d)\n",
			event.cursor_x, event.cursor_y);
		goto out;
	}
	if (event.cursor_flags & DRM_CASTKMS_CURSOR_IMAGE_CHANGED) {
		fprintf(stderr,
			"IMAGE_CHANGED set on position-only move\n");
		goto out;
	}
	printf("cursor_move=pass\n");

	/* Change cursor image: serial must increment, IMAGE_CHANGED set */
	for (uint32_t i = 0; i < cursor_w * cursor_h; i++)
		((uint32_t *)cursor_pixels)[i] = 0xFF00FF00;
	{
		struct drm_mode_cursor2 cur = {
			.flags = DRM_MODE_CURSOR_BO,
			.crtc_id = crtc_id,
			.width = cursor_w, .height = cursor_h,
			.handle = cursor_dumb.handle,
		};
		if (ioctl(kms_fd, DRM_IOCTL_MODE_CURSOR2, &cur) < 0) {
			perror("update cursor image");
			goto out;
		}
	}
	ioctl_ret = queue_capture_when_available(grant_fd, stream.stream_id,
		buffer_id, DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC,
		stream.mode_generation, user_data + 3, 0, 0);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("queue capture after image change");
		goto out;
	}
	if (read_capture_event(grant_fd, &event) ||
	    validate_capture_event(&event, stream.stream_id, buffer_id,
				   user_data + 3, stream.mode_generation,
				   width, height, sequence, false))
		goto out;
	sequence = event.sequence;
	if (!(event.cursor_flags & DRM_CASTKMS_CURSOR_IMAGE_CHANGED)) {
		fprintf(stderr,
			"IMAGE_CHANGED not set after cursor image update\n");
		goto out;
	}
	if (event.cursor_serial == first_serial) {
		fprintf(stderr,
			"cursor_serial did not change after image update\n");
		goto out;
	}
	last_visible_serial = event.cursor_serial;
	printf("cursor_image_changed=pass\n");

	/* Clear cursor: hidden transition has a new serial and no bitmap. */
	{
		struct drm_mode_cursor2 cur = {
			.flags = DRM_MODE_CURSOR_BO,
			.crtc_id = crtc_id,
			.width = cursor_w, .height = cursor_h,
			.handle = 0,
		};
		if (ioctl(kms_fd, DRM_IOCTL_MODE_CURSOR2, &cur) < 0) {
			perror("clear cursor");
			goto out;
		}
	}
	ioctl_ret = queue_capture_when_available(grant_fd, stream.stream_id,
		buffer_id, DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC,
		stream.mode_generation, user_data + 4, 0, 0);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("queue capture after clear");
		goto out;
	}
	if (read_capture_event(grant_fd, &event) ||
	    validate_capture_event(&event, stream.stream_id, buffer_id,
				   user_data + 4, stream.mode_generation,
				   width, height, sequence, false))
		goto out;
	if (!event.cursor_serial || event.cursor_serial == last_visible_serial) {
		fprintf(stderr, "cursor hide did not advance the serial: %u\n",
			event.cursor_serial);
		goto out;
	}
	if (event.cursor_flags & DRM_CASTKMS_CURSOR_VISIBLE) {
		fprintf(stderr, "VISIBLE set after cursor clear\n");
		goto out;
	}
	if (!(event.cursor_flags & DRM_CASTKMS_CURSOR_IMAGE_CHANGED) ||
	    event.cursor_width || event.cursor_height) {
		fprintf(stderr, "cursor hide transition metadata is invalid\n");
		goto out;
	}
	{
		struct drm_castkms_capture_read_cursor_bitmap bmp = {
			.stream_id = stream.stream_id,
			.buffer_id = buffer_id,
		};

		if (ioctl(grant_fd,
			  DRM_IOCTL_CASTKMS_CAPTURE_READ_CURSOR_BITMAP,
			  &bmp) < 0) {
			perror("read hidden cursor bitmap");
			goto out;
		}
		if (bmp.format || bmp.width || bmp.height || bmp.stride ||
		    bmp.bitmap_size) {
			fprintf(stderr, "hidden cursor retained stale bitmap data\n");
			goto out;
		}
	}
	printf("cursor_hidden_bitmap=pass\n");
	printf("cursor_clear=pass\n");

	printf("cursor_test=pass\n");
	ret = EXIT_SUCCESS;

out:
	if (second_buffer_id && stream.stream_id)
		unregister_capture_buffer(grant_fd, stream.stream_id,
					  second_buffer_id);
	if (buffer_id && stream.stream_id)
		unregister_capture_buffer(grant_fd, stream.stream_id, buffer_id);
	if (stream.stream_id)
		stop_capture(grant_fd, stream.stream_id);
	destroy_test_framebuffer(grant_fd, &capture_buf);
	destroy_test_framebuffer(grant_fd, &second_capture_buf);
	if (cursor_pixels)
		munmap(cursor_pixels, cursor_dumb.size);
	if (cursor_dumb.handle) {
		struct drm_mode_destroy_dumb destroy = {
			.handle = cursor_dumb.handle,
		};
		ioctl(kms_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	}
	if (attached) {
		ioctl_ret = detach_monitor(grant_fd, connector_id);
		if (ioctl_ret && ret == EXIT_SUCCESS) {
			errno = -ioctl_ret;
			perror("DETACH_MONITOR");
			ret = EXIT_FAILURE;
		}
	}
	if (kms_fd >= 0)
		close(kms_fd);
	if (grant_fd >= 0)
		close(grant_fd);
	return ret;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s [--grant-fd FD] [--deliver-one|--cursor|--mode-generation] DRM-DEVICE CRTC-ID\n",
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
	     !strcmp(argv[argument], "--cursor") ||
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
	if (mode && !strcmp(mode, "--cursor"))
		return run_cursor_test(device, inherited_fd, crtc_id);
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
		DRM_CASTKMS_GRANT_MANAGE_ATTACHMENT |
		DRM_CASTKMS_GRANT_UPDATE_EDID |
		DRM_CASTKMS_GRANT_READ_CURSOR, true);
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
		ioctl_ret = set_output_edid(fd, connector_id, edid,
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
		if (ioctl_ret != -EACCES) {
			fprintf(stderr,
				"ordinary-fd attach returned %d, expected %d\n",
				ioctl_ret, -EACCES);
			goto out_close;
		}
		ioctl_ret = detach_monitor(competitor_fd, connector_id);
		if (ioctl_ret != -EACCES) {
			fprintf(stderr,
				"ordinary-fd detach returned %d, expected %d\n",
				ioctl_ret, -EACCES);
			goto out_close;
		}
		printf("capture_attach_monitor=pass\n");
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
		ioctl_ret = set_output_edid(fd, connector_id, edid,
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
		ioctl_ret = set_output_edid(fd, connector_id, edid,
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
		ioctl_ret = set_output_edid(fd, connector_id, edid, 100);
		if (ioctl_ret != -EINVAL) {
			fprintf(stderr,
				"bad EDID size returned %d, expected %d\n",
				ioctl_ret, -EINVAL);
			goto out_close;
		}
		ioctl_ret = set_output_edid(competitor_fd, connector_id, edid,
					    sizeof(edid));
		if (ioctl_ret != -EACCES) {
			fprintf(stderr,
				"ordinary-fd EDID set returned %d, expected %d\n",
				ioctl_ret, -EACCES);
			goto out_close;
		}
		ioctl_ret = set_output_edid(fd, connector_id, NULL, 0);
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
		ioctl_ret = set_output_edid(fd, connector_id, edid,
					    sizeof(edid));
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("restore output EDID");
			goto out_close;
		}
		printf("capture_output_edid=pass\n");
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
			DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC, 0, 0,
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
			DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC, 0, 0,
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
		ioctl_ret = start_capture(fd, crtc_id, &second_stream);
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("start capture stream before detach");
			goto out_close;
		}

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
		ioctl_ret = stop_capture(fd, second_stream.stream_id);
		if (ioctl_ret != -ENOENT) {
			fprintf(stderr,
				"detached stream stop returned %d, expected %d\n",
				ioctl_ret, -ENOENT);
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
