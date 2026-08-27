// SPDX-License-Identifier: GPL-2.0-only

#ifndef PW_CASTKMS_H
#define PW_CASTKMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drm/castkms_drm.h>

#include <pipewire/pipewire.h>

#define PW_CASTKMS_BUFFER_LIMIT 4U
#ifndef PW_CASTKMS_HAS_EXPLICIT_SYNC
#define PW_CASTKMS_HAS_EXPLICIT_SYNC PW_CHECK_VERSION(1, 4, 8)
#endif
#define PW_CASTKMS_MAX_CURSOR_BITMAP_SIZE \
	(DRM_CASTKMS_CAPTURE_MAX_CURSOR_WIDTH * \
	 DRM_CASTKMS_CAPTURE_MAX_CURSOR_HEIGHT * 4U)

/*
 * A destination moves through this cycle:
 *
 *   IN_PIPEWIRE -> AVAILABLE -> QUEUED -> READY -> IN_PIPEWIRE
 *
 * PipeWire returns an IN_PIPEWIRE buffer to the producer.  CastKMS captures
 * into an AVAILABLE buffer after it is QUEUED.  A frame event makes it READY,
 * and publishing the completed buffer gives it back to PipeWire.
 */
enum capture_buffer_state {
	CAPTURE_BUFFER_IN_PIPEWIRE,
	CAPTURE_BUFFER_AVAILABLE,
	CAPTURE_BUFFER_QUEUED,
	CAPTURE_BUFFER_READY,
};

struct capture_damage {
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
};

struct capture_cursor {
	uint32_t serial;
	uint32_t flags;
	int32_t x;
	int32_t y;
	uint32_t hotspot_x;
	uint32_t hotspot_y;
	uint32_t width;
	uint32_t height;
};

struct captured_frame {
	uint64_t sequence;
	int64_t timestamp_ns;
	uint32_t flags;
	uint32_t dropped_frames;
	struct capture_damage damage;
	struct capture_cursor cursor;
};

struct capture_buffer {
	/* GEM and framebuffer objects live in the DRM file's private namespace. */
	uint32_t gem_handle;
	uint32_t fb_id;
	uint32_t pitch;
	uint64_t size;
	int dmabuf_fd;

	/* CastKMS registration and queue identity. */
	uint32_t buffer_id;
	uint64_t user_data;

	/* Optional acquire/release timelines exported to PipeWire. */
	uint32_t ready_syncobj;
	uint32_t reuse_syncobj;
	int ready_syncobj_fd;
	int reuse_syncobj_fd;
	uint64_t next_ready_point;
	uint64_t last_release_point;

	/* PipeWire association and the shared ownership state machine. */
	struct pw_buffer *pipewire_buffer;
	enum capture_buffer_state state;
	struct captured_frame frame;
};

struct pw_castkms {
	/* The opened primary node owns the DRM object namespace. */
	int drm_fd;
	char card_label[256];

	/* Selected connector and the compositor's active mode. */
	uint32_t connector_id;
	uint32_t crtc_id;
	char connector_name[64];
	uint32_t width;
	uint32_t height;
	uint32_t refresh;
	bool attached_here;

	/* Active CastKMS capture stream and its destination pool. */
	uint64_t capture_caps;
	uint32_t max_registered_buffers;
	uint32_t stream_id;
	uint64_t mode_generation;
	bool capture_active;
	bool supports_explicit_sync;
	bool restart_capture_on_buffer_add;
	struct capture_buffer buffers[PW_CASTKMS_BUFFER_LIMIT];
	uint32_t buffer_count;
	uint64_t user_data_sequence;

	/* PipeWire publisher and event-loop sources. */
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct spa_hook core_listener;
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct spa_source *drm_source;
	struct spa_source *process_timer;
	struct spa_source *sigint_source;
	struct spa_source *sigterm_source;

	/* Process lifetime and diagnostics. */
	bool failed;
	bool shutting_down;
	int exit_status;
	uint64_t frames_produced;

	/* Reusable staging allocation for READ_CURSOR_BITMAP. */
	void *cursor_bitmap;
	uint32_t cursor_bitmap_size;
	uint32_t cursor_bitmap_capacity;
	uint32_t cursor_bitmap_width;
	uint32_t cursor_bitmap_height;
	uint32_t cursor_bitmap_stride;
};

void pw_castkms_fail(struct pw_castkms *bridge, const char *operation,
		     int status);

/* CastKMS device, output, stream, and event handling. */
int castkms_open_device(struct pw_castkms *bridge, const char *device_path);
int castkms_configure_output(struct pw_castkms *bridge,
			     uint32_t preferred_crtc,
			     const void *edid, uint32_t edid_size);
int castkms_start_capture(struct pw_castkms *bridge);
int castkms_stop_capture(struct pw_castkms *bridge);
void castkms_close(struct pw_castkms *bridge);
void castkms_on_fd_ready(void *data, int fd, uint32_t mask);

/* DRM destination allocation and ownership transitions. */
struct capture_buffer *
castkms_find_buffer_by_pipewire(struct pw_castkms *bridge,
			       struct pw_buffer *pipewire_buffer);
struct capture_buffer *
castkms_find_buffer_by_id(struct pw_castkms *bridge, uint32_t buffer_id);
int castkms_create_destination(struct pw_castkms *bridge,
			       struct capture_buffer *buffer,
			       bool explicit_sync);
int castkms_destroy_destination(struct pw_castkms *bridge,
				struct capture_buffer *buffer);
void castkms_queue_available(struct pw_castkms *bridge);
int castkms_read_cursor_bitmap(struct pw_castkms *bridge,
			       const struct capture_buffer *buffer);
int castkms_signal_reuse_point(struct pw_castkms *bridge,
			       const struct capture_buffer *buffer,
			       uint64_t point);

/* PipeWire publication.  pw_init()/pw_deinit() remain owned by main(). */
int pipewire_open(struct pw_castkms *bridge);
int pipewire_run(struct pw_castkms *bridge);
void pipewire_close(struct pw_castkms *bridge);

#endif /* PW_CASTKMS_H */
