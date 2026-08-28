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
static_assert(sizeof(struct drm_castkms_get_grant) == 32,
	      "get-grant ABI size changed");
static_assert(sizeof(struct drm_event_castkms_capture_frame) == 112,
	      "capture-event ABI size changed");

static const uint32_t required_grant_rights =
	DRM_CASTKMS_GRANT_CAPTURE_PIXELS |
	DRM_CASTKMS_GRANT_MANAGE_ATTACHMENT |
	DRM_CASTKMS_GRANT_UPDATE_EDID |
	DRM_CASTKMS_GRANT_READ_CURSOR;

static int drain_stopped_stream_events(struct pw_castkms *bridge,
				       uint32_t stream_id);

/* ---- Grant validation -------------------------------------------------- */

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
	if (requested && *requested) {
		struct stat node;

		if (stat(requested, &node) || !S_ISCHR(node.st_mode) ||
		    drmGetNodeTypeFromDevId(node.st_rdev) != DRM_NODE_PRIMARY)
			return -EINVAL;
	} else {
		requested = "grant-fd";
	}

	if (snprintf(label, size, "%s", requested) >= (int)size)
		return -ENAMETOOLONG;

	return 0;
}

static int query_grant(struct pw_castkms *bridge)
{
	struct drm_castkms_get_grant grant = {};

	if (ioctl(bridge->grant_fd, DRM_IOCTL_CASTKMS_GET_GRANT, &grant) < 0)
		return -errno;

	if (!grant.grant_id || !grant.connector_id ||
	    (grant.rights & ~DRM_CASTKMS_GRANT_RIGHTS_MASK) ||
	    (grant.rights & required_grant_rights) != required_grant_rights ||
	    (grant.flags & ~DRM_CASTKMS_GRANT_FLAGS_MASK) ||
	    grant.state > DRM_CASTKMS_GRANT_STATE_REVOKED ||
	    grant.reserved)
		return -EPROTO;
	if (grant.state == DRM_CASTKMS_GRANT_STATE_REVOKED)
		return -EKEYREVOKED;

	bridge->grant_id = grant.grant_id;
	bridge->connector_id = grant.connector_id;
	bridge->grant_rights = grant.rights;
	bridge->grant_state = grant.state;
	bridge->output_index = grant.output_index;
	return 0;
}

int castkms_open_grant(struct pw_castkms *bridge, int inherited_fd,
		       const char *card_label)
{
	int flags;
	int status;

	bridge->grant_fd = fcntl(inherited_fd, F_DUPFD_CLOEXEC, 3);
	if (bridge->grant_fd < 0) {
		perror("duplicate grant fd");
		return -errno;
	}

	status = check_castkms_driver(bridge->grant_fd);
	if (status) {
		fprintf(stderr, "grant fd is not a CastKMS file\n");
		return status;
	}

	flags = fcntl(bridge->grant_fd, F_GETFL);
	if (flags < 0) {
		perror("inspect grant fd");
		return -errno;
	}
	if (!(flags & O_NONBLOCK)) {
		fprintf(stderr,
			"grant fd must be created with fd_flags=O_NONBLOCK\n");
		return -EINVAL;
	}

	status = query_grant(bridge);
	if (status) {
		fprintf(stderr, "inherited fd is not a usable display grant: %s\n",
			strerror(-status));
		return status;
	}

	status = set_card_label(card_label, bridge->card_label,
				sizeof(bridge->card_label));
	if (status) {
		fprintf(stderr, "%s\n", card_label && *card_label ?
			"-d does not name a DRM primary node" :
			"cannot label the grant's CastKMS device");
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

static int describe_grant_connector(struct pw_castkms *bridge,
				    uint32_t preferred_crtc,
				    uint32_t *candidate_crtc)
{
	drmModeRes *resources = drmModeGetResources(bridge->grant_fd);
	int status = -ENOENT;
	int i;

	if (!resources) {
		perror("drmModeGetResources");
		return errno ? -errno : -EIO;
	}

	for (i = 0; i < resources->count_connectors; i++) {
		drmModeConnector *connector = drmModeGetConnector(
			bridge->grant_fd, resources->connectors[i]);

		if (!connector)
			continue;
		if (connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
			drmModeFreeConnector(connector);
			continue;
		}
		if (connector->connector_id != bridge->connector_id) {
			drmModeFreeConnector(connector);
			continue;
		}

		if (preferred_crtc && !connector_can_drive_crtc(
					bridge->grant_fd, resources, connector,
					preferred_crtc)) {
			fprintf(stderr,
				"grant connector cannot drive requested CRTC %u\n",
				preferred_crtc);
			status = -EINVAL;
			goto out_connector;
		}

		*candidate_crtc = preferred_crtc ? preferred_crtc :
			first_compatible_crtc(bridge->grant_fd, resources,
					      connector);
		if (!*candidate_crtc) {
			fprintf(stderr, "grant connector has no possible CRTC\n");
			status = -ENOLINK;
			goto out_connector;
		}

		(void)snprintf(
			bridge->connector_name, sizeof(bridge->connector_name),
			"%s-%u",
			drmModeGetConnectorTypeName(connector->connector_type),
			connector->connector_type_id);
		status = 0;

out_connector:
		drmModeFreeConnector(connector);
		break;
	}

	drmModeFreeResources(resources);
	if (status == -ENOENT) {
		fprintf(stderr, "grant connector %u is not present\n",
			bridge->connector_id);
	}
	return status;
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

	if (ioctl(bridge->grant_fd, DRM_IOCTL_CASTKMS_CAPTURE_QUERY_CAPS,
		  &query) < 0) {
		perror("DRM_IOCTL_CASTKMS_CAPTURE_QUERY_CAPS");
		return -errno;
	}
	if (query.uapi_major != DRM_CASTKMS_CAPTURE_UAPI_MAJOR ||
	    query.uapi_minor < DRM_CASTKMS_CAPTURE_UAPI_MINOR ||
	    !(query.flags & DRM_CASTKMS_CAPTURE_CAP_GRANT_FD) ||
	    !(query.flags & DRM_CASTKMS_CAPTURE_CAP_GRANT_CONTROL_FD) ||
	    !(query.flags & DRM_CASTKMS_CAPTURE_CAP_IMPLICIT_SYNC) ||
	    !query.format_count || query.format_count > 256 ||
	    query.max_registered_buffers < 2 || query.reserved) {
		fprintf(stderr,
			"CastKMS capture UAPI %u.%u with GRANT_FD, "
			"GRANT_CONTROL_FD, and implicit sync is required\n",
			DRM_CASTKMS_CAPTURE_UAPI_MAJOR,
			DRM_CASTKMS_CAPTURE_UAPI_MINOR);
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
	if (ioctl(bridge->grant_fd, DRM_IOCTL_CASTKMS_CAPTURE_QUERY_CAPS,
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
		drmModeGetConnector(bridge->grant_fd, bridge->connector_id);
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

	encoder = drmModeGetEncoder(bridge->grant_fd, connector->encoder_id);
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

	crtc = drmModeGetCrtc(bridge->grant_fd, crtc_id);
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
#if PW_CASTKMS_HAS_EXPLICIT_SYNC
	uint64_t syncobj_cap = 0;
#endif
	int status;

	status = describe_grant_connector(bridge, preferred_crtc,
					    &candidate_crtc);
	if (status)
		return status;
	status = query_capture_caps(bridge, candidate_crtc);
	if (status)
		return status;

	status = attach_monitor(bridge->grant_fd, bridge->connector_id,
				edid, edid_size);
	if (status == -EBUSY) {
		status = update_output_edid(bridge->grant_fd,
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
	if (!status)
		status = query_grant(bridge);
	if (status || bridge->grant_state != DRM_CASTKMS_GRANT_STATE_ACTIVE) {
		fprintf(stderr, "grant did not become capture-active\n");
		return status ? status : -EAGAIN;
	}

#if PW_CASTKMS_HAS_EXPLICIT_SYNC
	if (!drmGetCap(bridge->grant_fd, DRM_CAP_SYNCOBJ_TIMELINE,
		       &syncobj_cap) && syncobj_cap &&
	    (bridge->capture_caps &
	     DRM_CASTKMS_CAPTURE_CAP_SYNCOBJ_TIMELINE)) {
		bridge->supports_explicit_sync = true;
		fprintf(stderr, "explicit sync enabled\n");
	}
#endif

	return 0;
}

/* ---- Capture stream lifetime ------------------------------------------ */

int castkms_start_capture(struct pw_castkms *bridge)
{
	struct drm_castkms_capture_start args = {
		.crtc_id = bridge->crtc_id,
		.flags = DRM_CASTKMS_CAPTURE_START_EXCLUSIVE |
			 DRM_CASTKMS_CAPTURE_START_EXCLUDE_CURSOR,
	};

	if (ioctl(bridge->grant_fd, DRM_IOCTL_CASTKMS_CAPTURE_START, &args) < 0) {
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
	status = ioctl(bridge->grant_fd, DRM_IOCTL_CASTKMS_CAPTURE_STOP, &args) < 0 ?
		-errno : 0;
	if (status && status != -EKEYREVOKED && status != -ENODEV)
		fprintf(stderr, "STOP_CAPTURE: %s\n", strerror(-status));
	bridge->capture_active = false;

	/* STOP destroys every registration belonging to the old stream. */
	for (i = 0; i < bridge->buffer_count; i++) {
		bridge->buffers[i].buffer_id = 0;
		bridge->buffers[i].user_data = 0;
	}

	if (!status)
		status = drain_stopped_stream_events(bridge, stopped_stream_id);
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

	if (bridge->attached_here && bridge->grant_fd >= 0) {
		status = detach_monitor(bridge->grant_fd, bridge->connector_id);
		if (status && status != -EKEYREVOKED && status != -ENODEV)
			fprintf(stderr, "DETACH_MONITOR: %s\n",
				strerror(-status));
		bridge->attached_here = false;
	}

	free(bridge->cursor_bitmap);
	bridge->cursor_bitmap = NULL;
	bridge->cursor_bitmap_size = 0;
	bridge->cursor_bitmap_capacity = 0;
	if (bridge->grant_fd >= 0)
		close(bridge->grant_fd);
	bridge->grant_fd = -1;
}

/* ---- DRM event validation and dispatch -------------------------------- */

static bool cursor_event_is_valid(
	const struct drm_event_castkms_capture_frame *event)
{
	if (event->cursor_flags & ~(DRM_CASTKMS_CURSOR_VISIBLE |
				      DRM_CASTKMS_CURSOR_IMAGE_CHANGED))
		return false;
	if (!event->cursor_serial)
		return !event->cursor_flags && !event->cursor_width &&
			event->cursor_height == 0;
	if (!(event->cursor_flags & DRM_CASTKMS_CURSOR_VISIBLE))
		return !event->cursor_width && !event->cursor_height;

	return event->cursor_width && event->cursor_height &&
		event->cursor_width <= DRM_CASTKMS_CAPTURE_MAX_CURSOR_WIDTH &&
		event->cursor_height <= DRM_CASTKMS_CAPTURE_MAX_CURSOR_HEIGHT &&
		event->cursor_hotspot_x < event->cursor_width &&
		event->cursor_hotspot_y < event->cursor_height;
}

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
		bridge->height - (uint32_t)event->damage_y ||
	    !cursor_event_is_valid(event))
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
		"damage=(%d,%d)+%ux%u/%ux%u cursor=%u/%#x "
		"pos=(%d,%d) hotspot=%u,%u size=%ux%u\n",
		(unsigned long long)event->mode_generation,
		(unsigned long long)bridge->mode_generation,
		event->flags, event->damage_x, event->damage_y,
		event->damage_width, event->damage_height,
		bridge->width, bridge->height, event->cursor_serial,
		event->cursor_flags, event->cursor_x, event->cursor_y,
		event->cursor_hotspot_x, event->cursor_hotspot_y,
		event->cursor_width, event->cursor_height);
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
		.damage = {
			.x = event->damage_x,
			.y = event->damage_y,
			.width = event->damage_width,
			.height = event->damage_height,
		},
		.cursor = {
			.serial = event->cursor_serial,
			.flags = event->cursor_flags,
			.x = event->cursor_x,
			.y = event->cursor_y,
			.hotspot_x = event->cursor_hotspot_x,
			.hotspot_y = event->cursor_hotspot_y,
			.width = event->cursor_width,
			.height = event->cursor_height,
		},
	};
	buffer->state = CAPTURE_BUFFER_READY;
	*frame_ready = true;
	return true;
}

static void handle_revoked_event(
	struct pw_castkms *bridge,
	const struct drm_event_castkms_grant_revoked *event)
{
	if (event->base.length != sizeof(*event) ||
	    event->grant_id != bridge->grant_id || event->status >= 0) {
		pw_castkms_fail(bridge, "invalid grant-revoked event", -EPROTO);
		return;
	}

	pw_castkms_fail(bridge, "capture grant revoked", event->status);
}

static bool handle_grant_state_event(
	struct pw_castkms *bridge,
	const struct drm_event_castkms_grant_state *event,
	bool *became_inactive)
{
	if (event->base.length != sizeof(*event) ||
	    event->grant_id != bridge->grant_id ||
	    event->state > DRM_CASTKMS_GRANT_STATE_REVOKED ||
	    event->reserved) {
		pw_castkms_fail(bridge, "invalid grant-state event", -EPROTO);
		return false;
	}

	bridge->grant_state = event->state;
	*became_inactive |= event->state != DRM_CASTKMS_GRANT_STATE_ACTIVE;
	return true;
}

static int dispatch_event_batch(struct pw_castkms *bridge, int fd,
				uint32_t discarded_stream_id,
				bool *frame_ready)
{
	uint64_t aligned_events[4096 / sizeof(uint64_t)];
	char *events = (char *)aligned_events;
	bool grant_became_inactive = false;
	ssize_t length;
	ssize_t offset;

	*frame_ready = false;
	length = read(fd, events, sizeof(aligned_events));
	if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return -EAGAIN;
	if (length <= 0) {
		int status = length < 0 ? -errno : -EIO;

		pw_castkms_fail(bridge, "DRM grant event read failed",
				 status);
		return status;
	}

	for (offset = 0; offset < length;) {
		struct drm_event *base = (struct drm_event *)(events + offset);

		if (length - offset < (ssize_t)sizeof(*base) ||
		    base->length < sizeof(*base) ||
		    (ssize_t)base->length > length - offset) {
			pw_castkms_fail(bridge, "malformed DRM event stream",
					 -EPROTO);
			return -EPROTO;
		}

		switch (base->type) {
		case DRM_CASTKMS_CAPTURE_EVENT_FRAME: {
			bool event_frame_ready;

			if (!handle_frame_event(bridge, (void *)base,
						discarded_stream_id,
						&event_frame_ready))
				return -EPROTO;
			*frame_ready |= event_frame_ready;
			break;
		}
		case DRM_CASTKMS_CAPTURE_EVENT_GRANT_REVOKED:
			handle_revoked_event(bridge, (void *)base);
			return -EKEYREVOKED;
		case DRM_CASTKMS_CAPTURE_EVENT_GRANT_STATE:
			if (!handle_grant_state_event(bridge, (void *)base,
						      &grant_became_inactive))
				return -EPROTO;
			break;
		default:
			pw_castkms_fail(bridge, "unexpected DRM event type",
					 -EPROTO);
			return -EPROTO;
		}

		offset += base->length;
	}

	/* Re-query after an inactive event to close a suspend/reacquire race. */
	if (grant_became_inactive &&
	    bridge->grant_state != DRM_CASTKMS_GRANT_STATE_ACTIVE) {
		int status = query_grant(bridge);

		if (status ||
		    bridge->grant_state != DRM_CASTKMS_GRANT_STATE_ACTIVE) {
			pw_castkms_fail(
				bridge,
				"capture grant suspended; restart required",
				status ? status : -EAGAIN);
			return status ? status : -EAGAIN;
		}
	}

	return 0;
}

static int drain_stopped_stream_events(struct pw_castkms *bridge,
				       uint32_t stream_id)
{
	for (;;) {
		bool ignored;
		int status = dispatch_event_batch(
			bridge, bridge->grant_fd, stream_id, &ignored);

		if (status == -EAGAIN)
			return 0;
		if (status)
			return status;
	}
}

void castkms_on_fd_ready(void *data, int fd, uint32_t mask)
{
	struct pw_castkms *bridge = data;
	bool frame_ready;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_castkms_fail(bridge, "DRM grant fd disconnected", -EIO);
		return;
	}
	if (dispatch_event_batch(bridge, fd, 0, &frame_ready))
		return;

	if (frame_ready && bridge->stream)
		(void)pw_stream_trigger_process(bridge->stream);
}
