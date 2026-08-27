// SPDX-License-Identifier: GPL-2.0-only

#include "pw-castkms.h"

#include <drm/castkms_drm.h>
#include <drm/drm_fourcc.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

static_assert(sizeof(struct drm_castkms_capture_query_caps) == 40,
		      "capture-query ABI size changed");
static_assert(sizeof(struct drm_event_castkms_capture_frame) == 112,
		      "capture-event ABI size changed");

/* ---- Device validation ------------------------------------------------- */

static int check_castkms_driver(int fd)
{
	static const char expected_name[] = "castkms";
	struct drm_version version = {};
	char name[32] = {};

	version.name = name;
	version.name_len = sizeof(name) - 1;
	if (ioctl(fd, DRM_IOCTL_VERSION, &version) < 0)
		return -errno;

	if (version.name_len != strlen(expected_name) ||
	    memcmp(name, expected_name, strlen(expected_name)))
		return -ENODEV;

	return 0;
}

static int set_card_label(const char *requested, char *label, size_t size)
{
	struct stat node;

	if (!requested || !*requested || stat(requested, &node) ||
	    !S_ISCHR(node.st_mode) ||
	    drmGetNodeTypeFromDevId(node.st_rdev) != DRM_NODE_PRIMARY)
		return -EINVAL;

	if (snprintf(label, size, "%s", requested) >= (int)size)
		return -ENAMETOOLONG;

	return 0;
}

static int find_castkms_card(char *path, size_t size)
{
	int i;

	for (i = 0; i < 16; i++) {
		int fd;
		int status;

		if (snprintf(path, size, "/dev/dri/card%d", i) >= (int)size)
			return -ENAMETOOLONG;
		fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
		if (fd < 0)
			continue;
		status = check_castkms_driver(fd);
		close(fd);
		if (!status)
			return 0;
	}

	fprintf(stderr, "no CastKMS primary node found\n");
	return -ENODEV;
}

int castkms_open_device(struct pw_castkms *bridge, const char *device_path)
{
	char discovered_path[256];
	const char *path = device_path;
	int status;

	if (!path || !*path) {
		status = find_castkms_card(discovered_path, sizeof(discovered_path));
		if (status)
			return status;
		path = discovered_path;
	}

	status = set_card_label(path, bridge->card_label,
				sizeof(bridge->card_label));
	if (status) {
		fprintf(stderr, "-d does not name a DRM primary node\n");
		return status;
	}

	bridge->drm_fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (bridge->drm_fd < 0) {
		perror(path);
		return -errno;
	}

	status = check_castkms_driver(bridge->drm_fd);
	if (status) {
		fprintf(stderr, "%s is not a CastKMS primary node\n", path);
		return status;
	}

	return 0;
}

/* ---- Output discovery and attachment ---------------------------------- */

static int crtc_index(const drmModeRes *resources, uint32_t crtc_id)
{
	int i;

	for (i = 0; i < resources->count_crtcs; i++) {
		if (resources->crtcs[i] == crtc_id)
			return i;
	}

	return -1;
}

static bool connector_can_drive_crtc(int fd, const drmModeRes *resources,
				     const drmModeConnector *connector,
				     uint32_t crtc_id)
{
	int index = crtc_index(resources, crtc_id);
	int i;

	if (index < 0 || index >= 32)
		return false;

	for (i = 0; i < connector->count_encoders; i++) {
		drmModeEncoder *encoder =
			drmModeGetEncoder(fd, connector->encoders[i]);
		bool possible;

		if (!encoder)
			continue;
		possible = (encoder->possible_crtcs & (1U << index)) != 0;
		drmModeFreeEncoder(encoder);
		if (possible)
			return true;
	}

	return false;
}

static uint32_t first_compatible_crtc(int fd, const drmModeRes *resources,
				      const drmModeConnector *connector)
{
	int i;

	for (i = 0; i < resources->count_crtcs; i++) {
		if (connector_can_drive_crtc(fd, resources, connector,
					     resources->crtcs[i]))
			return resources->crtcs[i];
	}

	return 0;
}

static int describe_device_connector(struct pw_castkms *bridge,
				     uint32_t preferred_crtc,
				     uint32_t *candidate_crtc)
{
	drmModeRes *resources = drmModeGetResources(bridge->drm_fd);
	drmModeConnector *selected = NULL;
	uint32_t selected_crtc = 0;
	int i;

	if (!resources) {
		perror("drmModeGetResources");
		return errno ? -errno : -EIO;
	}

	for (i = 0; i < resources->count_connectors; i++) {
		drmModeConnector *connector = drmModeGetConnector(
			bridge->drm_fd, resources->connectors[i]);
		uint32_t crtc;

		if (!connector)
			continue;
		if (connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
			drmModeFreeConnector(connector);
			continue;
		}
		crtc = preferred_crtc ? preferred_crtc :
			first_compatible_crtc(bridge->drm_fd, resources, connector);
		if (!crtc || (preferred_crtc && !connector_can_drive_crtc(
					bridge->drm_fd, resources, connector,
					preferred_crtc))) {
			drmModeFreeConnector(connector);
			continue;
		}

		if (!selected || connector->connection == DRM_MODE_DISCONNECTED) {
			drmModeFreeConnector(selected);
			selected = connector;
			selected_crtc = crtc;
			if (connector->connection == DRM_MODE_DISCONNECTED)
				break;
		} else {
			drmModeFreeConnector(connector);
		}
	}

	drmModeFreeResources(resources);
	if (!selected) {
		fprintf(stderr, "no display connector found\n");
		return -ENOENT;
	}

	bridge->connector_id = selected->connector_id;
	*candidate_crtc = selected_crtc;
	(void)snprintf(bridge->connector_name, sizeof(bridge->connector_name),
		       "%s-%u",
		       drmModeGetConnectorTypeName(selected->connector_type),
		       selected->connector_type_id);
	drmModeFreeConnector(selected);
	return 0;
}

static int query_capture_caps(struct pw_castkms *bridge, uint32_t crtc_id)
{
	struct drm_castkms_capture_query_caps query = {
		.crtc_id = crtc_id,
	};
	struct drm_castkms_capture_format *formats;
	uint32_t capacity;
	uint32_t max_registered_buffers;
	uint64_t caps;
	bool found_format = false;
	uint32_t i;
	int status = -EPROTO;

	if (ioctl(bridge->drm_fd, DRM_IOCTL_CASTKMS_CAPTURE_QUERY_CAPS,
		  &query) < 0) {
		perror("DRM_IOCTL_CASTKMS_CAPTURE_QUERY_CAPS");
		return -errno;
	}
	if (query.uapi_major != DRM_CASTKMS_CAPTURE_UAPI_MAJOR ||
	    query.uapi_minor < DRM_CASTKMS_CAPTURE_UAPI_MINOR ||
	    !(query.flags & DRM_CASTKMS_CAPTURE_CAP_IMPLICIT_SYNC) ||
	    !query.format_count || query.format_count > 256 ||
	    query.max_registered_buffers < 2 || query.reserved) {
		fprintf(stderr,
			"CastKMS capture UAPI 0.9 with implicit sync is required\n");
		return -EPROTO;
	}

	capacity = query.format_count;
	caps = query.flags;
	max_registered_buffers = query.max_registered_buffers;
	formats = calloc(capacity, sizeof(*formats));
	if (!formats)
		return -ENOMEM;

	query.formats_ptr = (uint64_t)(uintptr_t)formats;
	query.format_count = capacity;
	if (ioctl(bridge->drm_fd, DRM_IOCTL_CASTKMS_CAPTURE_QUERY_CAPS,
		  &query) < 0) {
		status = -errno;
		perror("DRM_IOCTL_CASTKMS_CAPTURE_QUERY_CAPS formats");
		goto out;
	}
	if (query.uapi_major != DRM_CASTKMS_CAPTURE_UAPI_MAJOR ||
	    query.uapi_minor < DRM_CASTKMS_CAPTURE_UAPI_MINOR ||
	    query.crtc_id != crtc_id || query.format_count > capacity ||
	    query.flags != caps ||
	    query.max_registered_buffers != max_registered_buffers ||
	    query.reserved)
		goto out;

	for (i = 0; i < query.format_count; i++) {
		if (formats[i].format == DRM_FORMAT_XRGB8888 &&
		    formats[i].modifier == DRM_FORMAT_MOD_LINEAR &&
		    !formats[i].flags) {
			found_format = true;
			break;
		}
	}
	if (!found_format) {
		fprintf(stderr, "linear XRGB8888 capture is not advertised\n");
		goto out;
	}

	bridge->capture_caps = query.flags;
	bridge->max_registered_buffers = query.max_registered_buffers;
	status = 0;

out:
	free(formats);
	return status;
}

static int attach_monitor(int fd, uint32_t connector_id, const void *edid,
			  uint32_t edid_size)
{
	struct drm_castkms_capture_attach_monitor args = {
		.connector_id = connector_id,
		.edid_size = edid_size,
		.edid_ptr = (uint64_t)(uintptr_t)edid,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_ATTACH_MONITOR, &args) < 0)
		return -errno;

	return 0;
}

static int update_output_edid(int fd, uint32_t connector_id,
			      const void *edid, uint32_t edid_size)
{
	struct drm_castkms_capture_set_output_edid args = {
		.connector_id = connector_id,
		.edid_size = edid_size,
		.edid_ptr = (uint64_t)(uintptr_t)edid,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_SET_OUTPUT_EDID, &args) < 0)
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

static int read_active_mode(struct pw_castkms *bridge,
			    uint32_t preferred_crtc)
{
	drmModeConnector *connector =
		drmModeGetConnector(bridge->drm_fd, bridge->connector_id);
	drmModeEncoder *encoder;
	drmModeCrtc *crtc;
	uint32_t crtc_id;

	if (!connector)
		return errno ? -errno : -EIO;
	if (connector->connection != DRM_MODE_CONNECTED ||
	    !connector->encoder_id) {
		drmModeFreeConnector(connector);
		return -ENOTCONN;
	}

	encoder = drmModeGetEncoder(bridge->drm_fd, connector->encoder_id);
	drmModeFreeConnector(connector);
	if (!encoder)
		return errno ? -errno : -EIO;
	if (!encoder->crtc_id) {
		drmModeFreeEncoder(encoder);
		return -ENOLINK;
	}

	crtc_id = encoder->crtc_id;
	drmModeFreeEncoder(encoder);
	if (preferred_crtc && crtc_id != preferred_crtc)
		return -ENOLINK;

	crtc = drmModeGetCrtc(bridge->drm_fd, crtc_id);
	if (!crtc)
		return errno ? -errno : -EIO;
	if (!crtc->mode_valid || !crtc->mode.hdisplay ||
	    !crtc->mode.vdisplay) {
		drmModeFreeCrtc(crtc);
		return -ENOLINK;
	}

	bridge->crtc_id = crtc->crtc_id;
	bridge->width = crtc->mode.hdisplay;
	bridge->height = crtc->mode.vdisplay;
	bridge->refresh = crtc->mode.vrefresh ? crtc->mode.vrefresh : 60;
	drmModeFreeCrtc(crtc);
	return 0;
}

static int wait_for_active_mode(struct pw_castkms *bridge,
				uint32_t preferred_crtc)
{
	int attempt;

	for (attempt = 0; attempt < 60; attempt++) {
		if (!read_active_mode(bridge, preferred_crtc))
			return 0;
		usleep(500000);
	}

	fprintf(stderr, "no active CRTC after attach\n");
	return -ETIMEDOUT;
}

int castkms_configure_output(struct pw_castkms *bridge,
			     uint32_t preferred_crtc,
			     const void *edid, uint32_t edid_size)
{
	uint32_t candidate_crtc = 0;
	int status;

	status = describe_device_connector(bridge, preferred_crtc,
					     &candidate_crtc);
	if (status)
		return status;
	status = query_capture_caps(bridge, candidate_crtc);
	if (status)
		return status;

	status = attach_monitor(bridge->drm_fd, bridge->connector_id,
				edid, edid_size);
	if (status == -EBUSY) {
		status = update_output_edid(bridge->drm_fd,
					    bridge->connector_id,
					    edid, edid_size);
		if (status) {
			fprintf(stderr, "SET_OUTPUT_EDID on existing attachment: %s\n",
				strerror(-status));
			return status;
		}
		fprintf(stderr, "using existing %s attachment\n",
			bridge->connector_name);
	} else if (status) {
		fprintf(stderr, "ATTACH_MONITOR: %s\n", strerror(-status));
		return status;
	} else {
		bridge->attached_here = true;
		fprintf(stderr, "attached %s with %u-byte EDID\n",
			bridge->connector_name, edid_size);
	}

	status = wait_for_active_mode(bridge, preferred_crtc);
	if (status)
		return status;
	if (!bridge->width || !bridge->height || !bridge->refresh ||
	    bridge->width > DRM_CASTKMS_CAPTURE_MAX_WIDTH ||
	    bridge->height > DRM_CASTKMS_CAPTURE_MAX_HEIGHT ||
	    (uint64_t)bridge->width * bridge->height * 4U > INT_MAX) {
		fprintf(stderr, "active mode cannot be represented by PipeWire\n");
		return -EOVERFLOW;
	}

	fprintf(stderr, "CRTC %u (%s): %ux%u@%u\n",
		bridge->crtc_id, bridge->connector_name,
		bridge->width, bridge->height, bridge->refresh);

	/* Attachment can activate a different route; validate the final CRTC. */
	status = query_capture_caps(bridge, bridge->crtc_id);
	if (status)
		return status;

	return 0;
}

/* ---- Capture stream lifetime ------------------------------------------ */

int castkms_start_capture(struct pw_castkms *bridge)
{
	struct drm_castkms_capture_start args = {
		.crtc_id = bridge->crtc_id,
		.flags = DRM_CASTKMS_CAPTURE_START_EXCLUSIVE,
	};

	if (ioctl(bridge->drm_fd, DRM_IOCTL_CASTKMS_CAPTURE_START, &args) < 0) {
		int status = -errno;

		fprintf(stderr, "START_CAPTURE: %s\n", strerror(-status));
		return status;
	}

	bridge->stream_id = args.stream_id;
	bridge->mode_generation = args.mode_generation;
	bridge->capture_active = true;
	fprintf(stderr, "capture stream %u, mode generation %llu\n",
		bridge->stream_id,
		(unsigned long long)bridge->mode_generation);
	return 0;
}

int castkms_stop_capture(struct pw_castkms *bridge)
{
	struct drm_castkms_capture_stop args;
	uint32_t stopped_stream_id;
	uint32_t i;
	int status;

	if (!bridge->capture_active)
		return 0;

	stopped_stream_id = bridge->stream_id;
	args = (struct drm_castkms_capture_stop) {
		.stream_id = stopped_stream_id,
	};
	status = ioctl(bridge->drm_fd, DRM_IOCTL_CASTKMS_CAPTURE_STOP, &args) < 0 ?
		-errno : 0;
	if (status && status != -ENODEV)
		fprintf(stderr, "STOP_CAPTURE: %s\n", strerror(-status));
	bridge->capture_active = false;

	/* STOP destroys every registration belonging to the old stream. */
	for (i = 0; i < bridge->buffer_count; i++) {
		bridge->buffers[i].buffer_id = 0;
		bridge->buffers[i].user_data = 0;
	}

	return status;
}

void castkms_close(struct pw_castkms *bridge)
{
	int status;

	(void)castkms_stop_capture(bridge);
	while (bridge->buffer_count) {
		int destroy_status;

		bridge->buffer_count--;
		destroy_status = castkms_destroy_destination(
			bridge, &bridge->buffers[bridge->buffer_count]);
		if (destroy_status)
			fprintf(stderr, "release capture destination: %s\n",
				strerror(-destroy_status));
	}

	if (bridge->attached_here && bridge->drm_fd >= 0) {
		status = detach_monitor(bridge->drm_fd, bridge->connector_id);
		if (status && status != -ENODEV)
			fprintf(stderr, "DETACH_MONITOR: %s\n",
				strerror(-status));
		bridge->attached_here = false;
	}

	if (bridge->drm_fd >= 0)
		close(bridge->drm_fd);
	bridge->drm_fd = -1;
}

/* ---- DRM event validation and dispatch -------------------------------- */

static bool frame_payload_is_valid(
	const struct pw_castkms *bridge,
	const struct drm_event_castkms_capture_frame *event)
{
	if (event->mode_generation != bridge->mode_generation ||
	    (event->flags & ~(DRM_CASTKMS_CAPTURE_FRAME_FULL_DAMAGE |
			      DRM_CASTKMS_CAPTURE_FRAME_MODE_CHANGED)) ||
	    event->damage_x < 0 || event->damage_y < 0 ||
	    !event->damage_width || !event->damage_height ||
	    (uint32_t)event->damage_x > bridge->width ||
	    event->damage_width > bridge->width - (uint32_t)event->damage_x ||
	    (uint32_t)event->damage_y > bridge->height ||
	    event->damage_height >
		bridge->height - (uint32_t)event->damage_y)
		return false;

	if ((event->flags & DRM_CASTKMS_CAPTURE_FRAME_FULL_DAMAGE) &&
	    (event->damage_x || event->damage_y ||
	     event->damage_width != bridge->width ||
	     event->damage_height != bridge->height))
		return false;

	return true;
}

static void log_invalid_frame(
	const struct pw_castkms *bridge,
	const struct drm_event_castkms_capture_frame *event)
{
	fprintf(stderr,
		"completion: generation=%llu/%llu flags=%#x "
		"damage=(%d,%d)+%ux%u/%ux%u\n",
		(unsigned long long)event->mode_generation,
		(unsigned long long)bridge->mode_generation,
		event->flags, event->damage_x, event->damage_y,
		event->damage_width, event->damage_height,
		bridge->width, bridge->height);
}

static bool handle_frame_event(
	struct pw_castkms *bridge,
	const struct drm_event_castkms_capture_frame *event,
	uint32_t discarded_stream_id, bool *frame_ready)
{
	struct capture_buffer *buffer;
	struct captured_frame *frame;

	*frame_ready = false;
	if (event->base.length != sizeof(*event)) {
		pw_castkms_fail(bridge, "invalid capture event size", -EPROTO);
		return false;
	}
	if (discarded_stream_id && event->stream_id == discarded_stream_id)
		return true;

	buffer = castkms_find_buffer_by_id(bridge, event->buffer_id);
	if (!buffer || buffer->state != CAPTURE_BUFFER_QUEUED ||
	    event->stream_id != bridge->stream_id ||
	    event->user_data != buffer->user_data || event->reserved) {
		fprintf(stderr,
			"event mismatch: buffer=%u found=%d state=%d "
			"stream=%u/%u user_data=%llu/%llu reserved=%u\n",
			event->buffer_id, buffer != NULL,
			buffer ? (int)buffer->state : -1,
			event->stream_id, bridge->stream_id,
			(unsigned long long)event->user_data,
			(unsigned long long)(buffer ? buffer->user_data : 0),
			event->reserved);
		pw_castkms_fail(bridge, "capture event ownership mismatch",
				 -EPROTO);
		return false;
	}

	if (event->status ||
	    (event->flags & DRM_CASTKMS_CAPTURE_FRAME_MODE_CHANGED)) {
		buffer->state = CAPTURE_BUFFER_AVAILABLE;
		pw_castkms_fail(bridge, "capture stream requires restart",
				event->status ? event->status : -ESTALE);
		return false;
	}
	if (!frame_payload_is_valid(bridge, event)) {
		log_invalid_frame(bridge, event);
		pw_castkms_fail(bridge, "invalid capture completion", -EPROTO);
		return false;
	}

	frame = &buffer->frame;
	*frame = (struct captured_frame) {
		.sequence = event->sequence,
		.timestamp_ns = event->timestamp_ns,
		.flags = event->flags,
		.dropped_frames = event->dropped_frames,
	};
	buffer->state = CAPTURE_BUFFER_READY;
	*frame_ready = true;
	return true;
}
