// SPDX-License-Identifier: GPL-2.0-only

#include <drm/castkms_drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../virtualscreen-edid.h"

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#define MAX_BUFFERS 4

enum buffer_state {
	BUF_FREE,
	BUF_QUEUED,
	BUF_COMPLETED,
	BUF_PW_OWNED,
};

struct capture_buffer {
	uint32_t gem_handle;
	uint32_t fb_id;
	uint32_t pitch;
	uint64_t size;
	int dmabuf_fd;
	uint32_t buffer_id;
	struct pw_buffer *pw_buf;
	enum buffer_state state;
	uint64_t sequence;
	int64_t timestamp_ns;
	uint32_t event_flags;
	uint32_t dropped_frames;
	int32_t damage_x;
	int32_t damage_y;
	uint32_t damage_width;
	uint32_t damage_height;
};

struct bridge {
	int drm_fd;
	char card_path[256];

	uint32_t crtc_id;
	uint32_t connector_id;
	char connector_name[64];
	uint32_t width;
	uint32_t height;
	uint32_t refresh;

	uint32_t stream_id;
	uint64_t mode_generation;
	bool capture_active;

	struct pw_main_loop *loop;
	struct pw_context *ctx;
	struct pw_core *core;
	struct spa_hook core_listener;
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct spa_source *drm_source;

	struct capture_buffer buffers[MAX_BUFFERS];
	int n_buffers;

	uint64_t user_data_seq;
	uint64_t frames_produced;
};

/* ---- DRM device discovery ---- */

static int check_castkms(int fd)
{
	struct drm_version version = {};
	char name[32] = {};

	version.name = name;
	version.name_len = sizeof(name) - 1;
	if (ioctl(fd, DRM_IOCTL_VERSION, &version) < 0)
		return -1;

	return (version.name_len == strlen("castkms") &&
		!memcmp(name, "castkms", strlen("castkms"))) ? 0 : -1;
}

static int find_castkms_card(char *path, size_t pathlen)
{
	for (int i = 0; i < 16; i++) {
		char devpath[64];
		int fd;

		snprintf(devpath, sizeof(devpath), "/dev/dri/card%d", i);
		fd = open(devpath, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;

		if (check_castkms(fd) == 0) {
			snprintf(path, pathlen, "%s", devpath);
			close(fd);
			return 0;
		}
		close(fd);
	}

	fprintf(stderr, "no castkms card found\n");
	return -1;
}

static int connector_can_drive_crtc(int drm_fd, drmModeConnector *conn,
				    uint32_t crtc_id)
{
	drmModeRes *res = drmModeGetResources(drm_fd);
	int crtc_idx = -1;
	int i;

	if (!res)
		return 0;

	for (i = 0; i < res->count_crtcs; i++) {
		if (res->crtcs[i] == crtc_id) {
			crtc_idx = i;
			break;
		}
	}
	if (crtc_idx < 0) {
		drmModeFreeResources(res);
		return 0;
	}

	for (i = 0; i < conn->count_encoders; i++) {
		drmModeEncoder *enc = drmModeGetEncoder(drm_fd, conn->encoders[i]);

		if (!enc)
			continue;
		if (enc->possible_crtcs & (1u << crtc_idx)) {
			drmModeFreeEncoder(enc);
			drmModeFreeResources(res);
			return 1;
		}
		drmModeFreeEncoder(enc);
	}

	drmModeFreeResources(res);
	return 0;
}

static int find_display_connector(int drm_fd, uint32_t crtc_id,
				  uint32_t *out_connector_id,
				  char *conn_name, size_t conn_name_len)
{
	drmModeRes *res = drmModeGetResources(drm_fd);
	drmModeConnector *fallback = NULL;
	uint32_t fallback_id = 0;

	if (!res) {
		perror("drmModeGetResources");
		return -1;
	}

	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *conn;

		conn = drmModeGetConnector(drm_fd, res->connectors[i]);
		if (!conn)
			continue;
		if (conn->connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
			drmModeFreeConnector(conn);
			continue;
		}
		if (crtc_id && !connector_can_drive_crtc(drm_fd, conn, crtc_id)) {
			drmModeFreeConnector(conn);
			continue;
		}

		if (conn->connection == DRM_MODE_DISCONNECTED) {
			*out_connector_id = conn->connector_id;
			snprintf(conn_name, conn_name_len, "%s-%u",
				 drmModeGetConnectorTypeName(conn->connector_type),
				 conn->connector_type_id);
			drmModeFreeConnector(conn);
			drmModeFreeResources(res);
			return 0;
		}

		if (!fallback) {
			fallback = conn;
			fallback_id = conn->connector_id;
			continue;
		}
		drmModeFreeConnector(conn);
	}

	if (fallback) {
		*out_connector_id = fallback_id;
		snprintf(conn_name, conn_name_len, "%s-%u",
			 drmModeGetConnectorTypeName(fallback->connector_type),
			 fallback->connector_type_id);
		drmModeFreeConnector(fallback);
		drmModeFreeResources(res);
		return 0;
	}

	drmModeFreeResources(res);
	fprintf(stderr, "no display connector found\n");
	return -1;
}

static int read_active_crtc(int drm_fd, uint32_t connector_id,
			    uint32_t prefer_crtc, uint32_t *out_crtc_id,
			    uint32_t *out_width, uint32_t *out_height,
			    uint32_t *out_refresh)
{
	drmModeConnector *conn = drmModeGetConnector(drm_fd, connector_id);
	drmModeEncoder *enc;
	drmModeCrtc *crtc;
	uint32_t crtc_id;

	if (!conn)
		return -1;
	if (conn->connection != DRM_MODE_CONNECTED || !conn->encoder_id) {
		drmModeFreeConnector(conn);
		return -1;
	}

	enc = drmModeGetEncoder(drm_fd, conn->encoder_id);
	drmModeFreeConnector(conn);
	if (!enc || !enc->crtc_id) {
		drmModeFreeEncoder(enc);
		return -1;
	}

	crtc_id = enc->crtc_id;
	drmModeFreeEncoder(enc);
	if (prefer_crtc && crtc_id != prefer_crtc)
		return -1;

	crtc = drmModeGetCrtc(drm_fd, crtc_id);
	if (!crtc || !crtc->mode_valid || !crtc->mode.hdisplay ||
	    !crtc->mode.vdisplay) {
		drmModeFreeCrtc(crtc);
		return -1;
	}

	*out_crtc_id = crtc->crtc_id;
	*out_width = crtc->mode.hdisplay;
	*out_height = crtc->mode.vdisplay;
	*out_refresh = crtc->mode.vrefresh ? crtc->mode.vrefresh : 60;
	drmModeFreeCrtc(crtc);
	return 0;
}

static int wait_active_crtc(int drm_fd, uint32_t connector_id,
			    uint32_t prefer_crtc, uint32_t *out_crtc_id,
			    uint32_t *out_width, uint32_t *out_height,
			    uint32_t *out_refresh)
{
	int attempt;

	for (attempt = 0; attempt < 60; attempt++) {
		if (!read_active_crtc(drm_fd, connector_id, prefer_crtc,
				      out_crtc_id, out_width, out_height,
				      out_refresh))
			return 0;
		usleep(500000);
	}

	fprintf(stderr, "no active CRTC after attach\n");
	return -1;
}

/* ---- castkms capture ioctls ---- */

static int capture_start(int fd, uint32_t crtc_id,
			  uint32_t *stream_id, uint64_t *mode_generation)
{
	struct drm_castkms_capture_start start = {
		.crtc_id = crtc_id,
		.flags = DRM_CASTKMS_CAPTURE_START_EXCLUSIVE,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_START, &start) < 0)
		return -errno;

	*stream_id = start.stream_id;
	*mode_generation = start.mode_generation;
	return 0;
}

static int capture_stop(int fd, uint32_t stream_id)
{
	struct drm_castkms_capture_stop stop = {
		.stream_id = stream_id,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_STOP, &stop) < 0)
		return -errno;

	return 0;
}

#define PW_EDID_BLOCK CASTKMS_EDID_BLOCK

static int fill_named_edid(uint8_t edid[PW_EDID_BLOCK], const char *name)
{
	return castkms_fill_named_edid(edid, name);
}

static int read_edid_file(const char *path, uint8_t **out, uint32_t *out_size)
{
	FILE *fp;
	uint8_t *data;
	long size;

	fp = fopen(path, "rb");
	if (!fp) {
		perror(path);
		return -1;
	}
	if (fseek(fp, 0, SEEK_END) || (size = ftell(fp)) < 0) {
		perror(path);
		fclose(fp);
		return -1;
	}
	rewind(fp);
	if (!size || size > 512 || size % PW_EDID_BLOCK) {
		fprintf(stderr,
			"%s: EDID must be a 128-byte multiple up to 512 bytes\n",
			path);
		fclose(fp);
		return -1;
	}
	data = malloc((size_t)size);
	if (!data) {
		fclose(fp);
		return -1;
	}
	if (fread(data, 1, (size_t)size, fp) != (size_t)size) {
		fprintf(stderr, "%s: short EDID read\n", path);
		free(data);
		fclose(fp);
		return -1;
	}
	fclose(fp);
	*out = data;
	*out_size = (uint32_t)size;
	return 0;
}

static int capture_set_output_edid(int fd, uint32_t stream_id,
				   const void *edid, uint32_t size)
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

static int capture_attach_monitor(int fd, uint32_t connector_id,
				  const void *edid, uint32_t size)
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

static int capture_register(int fd, uint32_t stream_id, uint32_t fb_id,
			     uint64_t mode_generation, uint32_t *buffer_id)
{
	struct drm_castkms_capture_register_buffer reg = {
		.stream_id = stream_id,
		.fb_id = fb_id,
		.flags = DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC,
		.mode_generation = mode_generation,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_REGISTER_BUFFER, &reg) < 0)
		return -errno;

	*buffer_id = reg.buffer_id;
	return 0;
}

static int capture_unregister(int fd, uint32_t stream_id, uint32_t buffer_id)
{
	struct drm_castkms_capture_unregister_buffer unreg = {
		.stream_id = stream_id,
		.buffer_id = buffer_id,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_UNREGISTER_BUFFER, &unreg) < 0)
		return -errno;

	return 0;
}

static int capture_queue(int fd, uint32_t stream_id, uint32_t buffer_id,
			  uint64_t mode_generation, uint64_t user_data)
{
	struct drm_castkms_capture_queue_buffer queue = {
		.stream_id = stream_id,
		.buffer_id = buffer_id,
		.flags = DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC,
		.mode_generation = mode_generation,
		.user_data = user_data,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_QUEUE_BUFFER, &queue) < 0)
		return -errno;

	return 0;
}

/* ---- Buffer management ---- */

static struct capture_buffer *find_buffer_by_pw(struct bridge *b,
						struct pw_buffer *pw)
{
	for (int i = 0; i < b->n_buffers; i++)
		if (b->buffers[i].pw_buf == pw)
			return &b->buffers[i];
	return NULL;
}

static struct capture_buffer *find_buffer_by_id(struct bridge *b,
						uint32_t buffer_id)
{
	for (int i = 0; i < b->n_buffers; i++)
		if (b->buffers[i].buffer_id == buffer_id)
			return &b->buffers[i];
	return NULL;
}

static int alloc_capture_buffer(struct bridge *b, struct capture_buffer *buf)
{
	struct drm_mode_create_dumb dumb = {
		.width = b->width,
		.height = b->height,
		.bpp = 32,
	};
	struct drm_mode_fb_cmd2 fb = {
		.width = b->width,
		.height = b->height,
		.pixel_format = DRM_FORMAT_XRGB8888,
	};
	struct drm_prime_handle prime = {
		.flags = DRM_CLOEXEC | DRM_RDWR,
	};
	struct drm_mode_destroy_dumb destroy;
	int ret;

	*buf = (struct capture_buffer){ .dmabuf_fd = -1 };

	if (ioctl(b->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) < 0) {
		perror("DRM_IOCTL_MODE_CREATE_DUMB");
		return -1;
	}

	buf->gem_handle = dumb.handle;
	buf->pitch = dumb.pitch;
	buf->size = dumb.size;

	fb.handles[0] = dumb.handle;
	fb.pitches[0] = dumb.pitch;
	if (ioctl(b->drm_fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0) {
		perror("DRM_IOCTL_MODE_ADDFB2");
		goto err_gem;
	}
	buf->fb_id = fb.fb_id;

	prime.handle = dumb.handle;
	if (ioctl(b->drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0) {
		perror("DRM_IOCTL_PRIME_HANDLE_TO_FD");
		goto err_fb;
	}
	buf->dmabuf_fd = prime.fd;

	ret = capture_register(b->drm_fd, b->stream_id, buf->fb_id,
			       b->mode_generation, &buf->buffer_id);
	if (ret) {
		errno = -ret;
		perror("REGISTER_BUFFER");
		goto err_prime;
	}

	buf->state = BUF_PW_OWNED;
	return 0;

err_prime:
	close(buf->dmabuf_fd);
	buf->dmabuf_fd = -1;
err_fb:
	ioctl(b->drm_fd, DRM_IOCTL_MODE_RMFB, &buf->fb_id);
	buf->fb_id = 0;
err_gem:
	destroy = (struct drm_mode_destroy_dumb){ .handle = buf->gem_handle };
	ioctl(b->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	buf->gem_handle = 0;
	return -1;
}

static void free_capture_buffer(struct bridge *b, struct capture_buffer *buf)
{
	struct drm_mode_destroy_dumb destroy;

	if (buf->buffer_id && b->capture_active)
		capture_unregister(b->drm_fd, b->stream_id, buf->buffer_id);

	if (buf->dmabuf_fd >= 0)
		close(buf->dmabuf_fd);

	if (buf->fb_id)
		ioctl(b->drm_fd, DRM_IOCTL_MODE_RMFB, &buf->fb_id);

	if (buf->gem_handle) {
		destroy = (struct drm_mode_destroy_dumb){
			.handle = buf->gem_handle,
		};
		ioctl(b->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	}

	*buf = (struct capture_buffer){ .dmabuf_fd = -1 };
}

static void submit_free_buffers(struct bridge *b)
{
	if (!b->capture_active)
		return;

	for (int i = 0; i < b->n_buffers; i++) {
		struct capture_buffer *buf = &b->buffers[i];
		int ret;

		if (buf->state != BUF_FREE)
			continue;

		ret = capture_queue(b->drm_fd, b->stream_id, buf->buffer_id,
				    b->mode_generation, ++b->user_data_seq);
		if (ret == -EBUSY)
			break;
		if (ret) {
			fprintf(stderr, "QUEUE_BUFFER: %s\n", strerror(-ret));
			continue;
		}

		buf->state = BUF_QUEUED;
	}
}

/* ---- DRM event handler ---- */

static void on_drm_readable(void *data, int fd, uint32_t mask)
{
	struct bridge *b = data;
	char event_buf[256];
	ssize_t len;

	(void)fd;
	(void)mask;

	len = read(b->drm_fd, event_buf, sizeof(event_buf));
	if (len <= 0)
		return;

	for (ssize_t off = 0; off < len;) {
		struct drm_event *base = (struct drm_event *)(event_buf + off);
		struct drm_event_castkms_capture_frame *ev;
		struct capture_buffer *buf;

		if (off + (ssize_t)base->length > len)
			break;

		if (base->type != DRM_CASTKMS_CAPTURE_EVENT_FRAME ||
		    base->length != sizeof(*ev)) {
			off += base->length;
			continue;
		}

		ev = (struct drm_event_castkms_capture_frame *)base;
		buf = find_buffer_by_id(b, ev->buffer_id);

		if (buf && buf->state == BUF_QUEUED) {
			buf->state = BUF_COMPLETED;
			buf->sequence = ev->sequence;
			buf->timestamp_ns = ev->timestamp_ns;
			buf->event_flags = ev->flags;
			buf->dropped_frames = ev->dropped_frames;
			buf->damage_x = ev->damage_x;
			buf->damage_y = ev->damage_y;
			buf->damage_width = ev->damage_width;
			buf->damage_height = ev->damage_height;
		}

		off += base->length;
	}

	if (b->stream)
		pw_stream_trigger_process(b->stream);
}

/* ---- PipeWire stream callbacks ---- */

static void on_state_changed(void *data, enum pw_stream_state old,
			      enum pw_stream_state state, const char *error)
{
	struct bridge *b = data;

	(void)old;
	fprintf(stderr, "stream: %s", pw_stream_state_as_string(state));
	if (error)
		fprintf(stderr, " (%s)", error);
	fprintf(stderr, "\n");

	switch (state) {
	case PW_STREAM_STATE_STREAMING:
		pw_stream_trigger_process(b->stream);
		break;
	case PW_STREAM_STATE_ERROR:
	case PW_STREAM_STATE_UNCONNECTED:
		pw_main_loop_quit(b->loop);
		break;
	default:
		break;
	}
}

static void on_param_changed(void *data, uint32_t id,
			      const struct spa_pod *param)
{
	struct bridge *b = data;
	uint8_t params_buf[1024];
	struct spa_pod_builder builder;
	const struct spa_pod *params[3];
	int n_params = 0;

	if (!param || id != SPA_PARAM_Format)
		return;

	spa_pod_builder_init(&builder, params_buf, sizeof(params_buf));

	params[n_params++] = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers,
			SPA_POD_CHOICE_RANGE_Int(MAX_BUFFERS, 2, MAX_BUFFERS),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size,
			SPA_POD_Int(b->width * b->height * 4),
		SPA_PARAM_BUFFERS_stride, SPA_POD_Int(b->width * 4),
		SPA_PARAM_BUFFERS_dataType,
			SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_DmaBuf));

	params[n_params++] = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size,
			SPA_POD_Int(sizeof(struct spa_meta_header)));

	params[n_params++] = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
		SPA_PARAM_META_size,
			SPA_POD_CHOICE_RANGE_Int(
				sizeof(struct spa_meta_region),
				sizeof(struct spa_meta_region),
				sizeof(struct spa_meta_region) * 16));

	pw_stream_update_params(b->stream, params, n_params);
}

static void on_add_buffer(void *data, struct pw_buffer *pw_buf)
{
	struct bridge *b = data;
	struct capture_buffer *buf;
	struct spa_buffer *spa_buf;
	struct spa_data *d;

	if (b->n_buffers >= MAX_BUFFERS) {
		fprintf(stderr, "too many buffers\n");
		return;
	}

	buf = &b->buffers[b->n_buffers];
	if (alloc_capture_buffer(b, buf) < 0) {
		fprintf(stderr, "buffer allocation failed\n");
		return;
	}

	buf->pw_buf = pw_buf;

	spa_buf = pw_buf->buffer;
	d = &spa_buf->datas[0];
	d->type = SPA_DATA_DmaBuf;
	d->fd = buf->dmabuf_fd;
	d->maxsize = buf->size;
	d->data = NULL;
	d->chunk->offset = 0;
	d->chunk->size = buf->size;
	d->chunk->stride = buf->pitch;

	b->n_buffers++;
}

static void on_remove_buffer(void *data, struct pw_buffer *pw_buf)
{
	struct bridge *b = data;
	struct capture_buffer *buf = find_buffer_by_pw(b, pw_buf);
	int idx;

	if (!buf)
		return;

	idx = buf - b->buffers;
	free_capture_buffer(b, buf);

	if (idx < b->n_buffers - 1)
		b->buffers[idx] = b->buffers[b->n_buffers - 1];
	b->n_buffers--;
}

static void on_process(void *data)
{
	struct bridge *b = data;
	struct pw_buffer *pw_buf;

	for (int i = 0; i < b->n_buffers; i++) {
		struct capture_buffer *buf = &b->buffers[i];
		struct spa_buffer *spa_buf;
		struct spa_meta_header *h;
		struct spa_meta *damage_meta;

		if (buf->state != BUF_COMPLETED)
			continue;

		spa_buf = buf->pw_buf->buffer;

		h = spa_buffer_find_meta_data(spa_buf, SPA_META_Header,
					      sizeof(*h));
		if (h) {
			h->pts = buf->timestamp_ns;
			h->dts_offset = 0;
			h->seq = buf->sequence;
			h->flags = 0;
		}

		damage_meta = spa_buffer_find_meta(spa_buf,
						   SPA_META_VideoDamage);
		if (damage_meta) {
			struct spa_meta_region *r;
			uint32_t n = 0;

			spa_meta_for_each(r, damage_meta) {
				if (n == 0 && buf->damage_width &&
				    buf->damage_height) {
					r->region.position.x = buf->damage_x;
					r->region.position.y = buf->damage_y;
					r->region.size.width = buf->damage_width;
					r->region.size.height = buf->damage_height;
				} else {
					r->region = SPA_REGION(0, 0, 0, 0);
				}
				n++;
			}
		}

		spa_buf->datas[0].chunk->offset = 0;
		spa_buf->datas[0].chunk->size = buf->size;
		spa_buf->datas[0].chunk->stride = buf->pitch;

		pw_stream_queue_buffer(b->stream, buf->pw_buf);
		buf->state = BUF_PW_OWNED;
		b->frames_produced++;
	}

	while ((pw_buf = pw_stream_dequeue_buffer(b->stream))) {
		struct capture_buffer *buf = find_buffer_by_pw(b, pw_buf);

		if (buf)
			buf->state = BUF_FREE;
	}

	submit_free_buffers(b);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed,
	.param_changed = on_param_changed,
	.add_buffer = on_add_buffer,
	.remove_buffer = on_remove_buffer,
	.process = on_process,
};

/* ---- PipeWire core events ---- */

static void on_core_error(void *data, uint32_t id, int seq,
			   int res, const char *message)
{
	struct bridge *b = data;

	(void)seq;
	fprintf(stderr, "core error id=%u: %s (%s)\n",
		id, message, spa_strerror(res));

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_main_loop_quit(b->loop);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

/* ---- Signal handling ---- */

static void on_signal(void *data, int signal_number)
{
	struct bridge *b = data;

	(void)signal_number;
	pw_main_loop_quit(b->loop);
}

/* ---- Main ---- */

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [-d /dev/dri/cardN] [-c crtc-id] [-e edid.bin] [-n name]\n"
		"  Default output name is \"VirtualScreen\" (EDID names are at most 13 characters).\n",
		prog);
}

int main(int argc, char *argv[])
{
	struct bridge bridge = { .drm_fd = -1 };
	struct bridge *b = &bridge;
	uint8_t format_buf[1024];
	struct spa_pod_builder builder;
	const struct spa_pod *params[1];
	struct pw_properties *props;
	char node_name[128];
	char crtc_str[16];
	uint32_t target_crtc = 0;
	bool crtc_specified = false;
	const char *edid_path = NULL;
	const char *edid_name = NULL;
	uint8_t named_edid[PW_EDID_BLOCK];
	uint8_t *edid = NULL;
	uint8_t *edid_alloc = NULL;
	uint32_t edid_size = 0;
	int ioctl_ret;
	int opt;
	int ret = EXIT_FAILURE;

	while ((opt = getopt(argc, argv, "d:c:e:n:h")) != -1) {
		switch (opt) {
		case 'd':
			snprintf(b->card_path, sizeof(b->card_path),
				 "%s", optarg);
			break;
		case 'c':
			target_crtc = strtoul(optarg, NULL, 0);
			crtc_specified = true;
			break;
		case 'e':
			edid_path = optarg;
			break;
		case 'n':
			edid_name = optarg;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}

	if (edid_path && edid_name) {
		fprintf(stderr, "-e and -n are mutually exclusive\n");
		return EXIT_FAILURE;
	}

	if (!b->card_path[0] &&
	    find_castkms_card(b->card_path, sizeof(b->card_path)))
		return EXIT_FAILURE;

	b->drm_fd = open(b->card_path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (b->drm_fd < 0) {
		perror(b->card_path);
		return EXIT_FAILURE;
	}

	if (check_castkms(b->drm_fd)) {
		fprintf(stderr, "%s: not a castkms device\n", b->card_path);
		goto out;
	}

	ioctl(b->drm_fd, DRM_IOCTL_DROP_MASTER, 0);

	if (edid_name) {
		if (fill_named_edid(named_edid, edid_name)) {
			fprintf(stderr,
				"monitor name must be at most 13 characters\n");
			goto out;
		}
		edid = named_edid;
		edid_size = sizeof(named_edid);
	} else if (edid_path) {
		if (read_edid_file(edid_path, &edid_alloc, &edid_size))
			goto out;
		edid = edid_alloc;
	} else if (fill_named_edid(named_edid, NULL)) {
		fprintf(stderr, "failed to build default output EDID\n");
		goto out;
	} else {
		edid = named_edid;
		edid_size = sizeof(named_edid);
	}

	if (find_display_connector(b->drm_fd,
				   crtc_specified ? target_crtc : 0,
				   &b->connector_id, b->connector_name,
				   sizeof(b->connector_name)))
		goto out;

	ioctl_ret = capture_attach_monitor(b->drm_fd, b->connector_id, edid,
					   edid_size);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("ATTACH_MONITOR");
		goto out;
	}
	fprintf(stderr, "attached %s with %u-byte EDID\n",
		b->connector_name, edid_size);

	if (wait_active_crtc(b->drm_fd, b->connector_id,
			     crtc_specified ? target_crtc : 0, &b->crtc_id,
			     &b->width, &b->height, &b->refresh))
		goto out;

	fprintf(stderr, "CRTC %u (%s): %ux%u@%u\n",
		b->crtc_id, b->connector_name,
		b->width, b->height, b->refresh);

	ioctl_ret = capture_start(b->drm_fd, b->crtc_id,
				  &b->stream_id, &b->mode_generation);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("START_CAPTURE");
		goto out;
	}
	b->capture_active = true;
	fprintf(stderr, "capture stream %u, mode generation %llu\n",
		b->stream_id, (unsigned long long)b->mode_generation);

	if (edid_size) {
		ioctl_ret = capture_set_output_edid(b->drm_fd, b->stream_id,
						    edid, edid_size);
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("SET_OUTPUT_EDID");
			goto out_capture;
		}
	}

	pw_init(&argc, &argv);

	b->loop = pw_main_loop_new(NULL);
	if (!b->loop) {
		fprintf(stderr, "pw_main_loop_new failed\n");
		goto out_capture;
	}

	pw_loop_add_signal(pw_main_loop_get_loop(b->loop), SIGINT,
			   on_signal, b);
	pw_loop_add_signal(pw_main_loop_get_loop(b->loop), SIGTERM,
			   on_signal, b);

	b->ctx = pw_context_new(pw_main_loop_get_loop(b->loop), NULL, 0);
	if (!b->ctx) {
		fprintf(stderr, "pw_context_new failed\n");
		goto out_loop;
	}

	b->core = pw_context_connect(b->ctx, NULL, 0);
	if (!b->core) {
		fprintf(stderr, "pw_context_connect: %s\n", strerror(errno));
		goto out_ctx;
	}

	pw_core_add_listener(b->core, &b->core_listener, &core_events, b);

	b->drm_source = pw_loop_add_io(pw_main_loop_get_loop(b->loop),
					b->drm_fd, SPA_IO_IN, false,
					on_drm_readable, b);
	if (!b->drm_source) {
		fprintf(stderr, "pw_loop_add_io failed\n");
		goto out_core;
	}

	{
		const char *card = strrchr(b->card_path, '/');

		card = card ? card + 1 : b->card_path;
		snprintf(node_name, sizeof(node_name),
			 "castkms.%.32s.crtc-%u", card, b->crtc_id);
	}

	snprintf(crtc_str, sizeof(crtc_str), "%u", b->crtc_id);

	props = pw_properties_new(
		PW_KEY_MEDIA_CLASS, "Video/Source",
		PW_KEY_NODE_NAME, node_name,
		PW_KEY_NODE_DESCRIPTION, b->connector_name,
		"device.api", "drm",
		"api.castkms.card", b->card_path,
		"api.castkms.crtc-id", crtc_str,
		"api.castkms.connector", b->connector_name,
		NULL);

	b->stream = pw_stream_new(b->core, node_name, props);
	if (!b->stream) {
		fprintf(stderr, "pw_stream_new failed\n");
		goto out_drm_source;
	}

	pw_stream_add_listener(b->stream, &b->stream_listener,
			       &stream_events, b);

	spa_pod_builder_init(&builder, format_buf, sizeof(format_buf));

	params[0] = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType,
			SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype,
			SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format,
			SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx),
		SPA_FORMAT_VIDEO_modifier,
			SPA_POD_Long(DRM_FORMAT_MOD_LINEAR),
		SPA_FORMAT_VIDEO_size,
			SPA_POD_Rectangle(&SPA_RECTANGLE(b->width, b->height)),
		SPA_FORMAT_VIDEO_framerate,
			SPA_POD_Fraction(&SPA_FRACTION(b->refresh, 1)));

	ioctl_ret = pw_stream_connect(b->stream, PW_DIRECTION_OUTPUT,
				      PW_ID_ANY,
				      PW_STREAM_FLAG_DRIVER |
				      PW_STREAM_FLAG_ALLOC_BUFFERS,
				      params, 1);
	if (ioctl_ret < 0) {
		fprintf(stderr, "pw_stream_connect: %s\n",
			spa_strerror(ioctl_ret));
		goto out_stream;
	}

	fprintf(stderr, "running\n");
	pw_main_loop_run(b->loop);

	fprintf(stderr, "produced %llu frames\n",
		(unsigned long long)b->frames_produced);
	ret = EXIT_SUCCESS;

out_stream:
	pw_stream_destroy(b->stream);
	b->stream = NULL;
out_drm_source:
	pw_loop_destroy_source(pw_main_loop_get_loop(b->loop), b->drm_source);
out_core:
	pw_core_disconnect(b->core);
out_ctx:
	pw_context_destroy(b->ctx);
out_loop:
	pw_main_loop_destroy(b->loop);
	pw_deinit();
out_capture:
	for (int i = 0; i < b->n_buffers; i++)
		free_capture_buffer(b, &b->buffers[i]);
	if (b->capture_active)
		capture_stop(b->drm_fd, b->stream_id);
out:
	free(edid_alloc);
	if (b->drm_fd >= 0)
		close(b->drm_fd);
	return ret;
}
