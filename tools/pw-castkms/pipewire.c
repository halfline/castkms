// SPDX-License-Identifier: GPL-2.0-only

#include "pw-castkms.h"

#include <drm/castkms_drm.h>
#include <drm/drm_fourcc.h>

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>


/* ---- Stream negotiation ------------------------------------------------ */

static void on_process_timer(void *data, uint64_t expirations)
{
	struct pw_castkms *bridge = data;

	(void)expirations;
	if (bridge->stream && !bridge->shutting_down)
		(void)pw_stream_trigger_process(bridge->stream);
}

static int update_process_timer(struct pw_castkms *bridge, bool running)
{
	struct timespec first = { .tv_nsec = 1 };
	struct timespec interval;
	uint64_t interval_ns;

	if (!bridge->process_timer)
		return -EINVAL;
	if (!running) {
		return pw_loop_update_timer(pw_main_loop_get_loop(bridge->loop),
					    bridge->process_timer,
					    NULL, NULL, false);
	}

	interval_ns = SPA_NSEC_PER_SEC / bridge->refresh;
	interval.tv_sec = interval_ns / SPA_NSEC_PER_SEC;
	interval.tv_nsec = interval_ns % SPA_NSEC_PER_SEC;
	return pw_loop_update_timer(pw_main_loop_get_loop(bridge->loop),
				    bridge->process_timer,
				    &first, &interval, false);
}

static void on_stream_state_changed(void *data, enum pw_stream_state old,
				    enum pw_stream_state state,
				    const char *error)
{
	struct pw_castkms *bridge = data;

	(void)old;
	fprintf(stderr, "stream: %s", pw_stream_state_as_string(state));
	if (error)
		fprintf(stderr, " (%s)", error);
	fprintf(stderr, "\n");

	switch (state) {
	case PW_STREAM_STATE_ERROR:
		(void)update_process_timer(bridge, false);
		pw_castkms_fail(bridge,
				error ? error : "PipeWire stream error", 0);
		break;
	case PW_STREAM_STATE_UNCONNECTED:
		(void)update_process_timer(bridge, false);
		if (!bridge->shutting_down)
			pw_castkms_fail(bridge, "PipeWire stream disconnected", 0);
		break;
	case PW_STREAM_STATE_PAUSED:
		(void)update_process_timer(bridge, false);
		break;
	case PW_STREAM_STATE_STREAMING:
		if (update_process_timer(bridge, true) < 0)
			pw_castkms_fail(bridge, "PipeWire timer setup failed", 0);
		break;
	default:
		break;
	}
}

static void on_stream_param_changed(void *data, uint32_t id,
				    const struct spa_pod *param)
{
	struct pw_castkms *bridge = data;
	struct spa_video_info_raw info = {};
	uint8_t params_buffer[1024];
	struct spa_pod_builder builder;
	const struct spa_pod *params[3];
	uint32_t media_type;
	uint32_t media_subtype;
	uint32_t frame_size;
	uint32_t stride = bridge->width * 4U;
	int buffer_count = (int)SPA_MIN(PW_CASTKMS_BUFFER_LIMIT,
					bridge->max_registered_buffers);
	int param_count = 0;
	int status;

	if (!param || id != SPA_PARAM_Format)
		return;
	if (spa_format_parse(param, &media_type, &media_subtype) < 0 ||
	    media_type != SPA_MEDIA_TYPE_video ||
	    media_subtype != SPA_MEDIA_SUBTYPE_raw ||
	    spa_format_video_raw_parse(param, &info) < 0 ||
	    info.format != SPA_VIDEO_FORMAT_BGRx ||
	    !(info.flags & SPA_VIDEO_FLAG_MODIFIER) ||
	    info.modifier != DRM_FORMAT_MOD_LINEAR ||
	    info.size.width != bridge->width ||
	    info.size.height != bridge->height ||
	    info.framerate.num != bridge->refresh ||
	    info.framerate.denom != 1) {
		pw_castkms_fail(bridge,
				"unsupported negotiated PipeWire format", -EPROTO);
		return;
	}
	frame_size = bridge->width * bridge->height * 4U;

	spa_pod_builder_init(&builder, params_buffer, sizeof(params_buffer));

	params[param_count++] = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers,
			SPA_POD_CHOICE_RANGE_Int(buffer_count, 2, buffer_count),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size, SPA_POD_Int(frame_size),
		SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
		SPA_PARAM_BUFFERS_dataType,
			SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_DmaBuf));

	params[param_count++] = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size,
			SPA_POD_Int(sizeof(struct spa_meta_header)));

	params[param_count++] = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
		SPA_PARAM_META_size,
			SPA_POD_CHOICE_RANGE_Int(
				sizeof(struct spa_meta_region),
				sizeof(struct spa_meta_region),
				sizeof(struct spa_meta_region) * 16));

	status = pw_stream_update_params(bridge->stream, params, param_count);
	if (status < 0)
		pw_castkms_fail(bridge, "pw_stream_update_params failed", status);
}

/* ---- PipeWire buffer adaptation --------------------------------------- */

static void configure_dmabuf_data(struct spa_data *data,
				  const struct capture_buffer *buffer)
{
	data->type = SPA_DATA_DmaBuf;
	data->fd = buffer->dmabuf_fd;
	data->flags = SPA_DATA_FLAG_READABLE | SPA_DATA_FLAG_MAPPABLE;
	data->mapoffset = 0;
	data->maxsize = buffer->size;
	data->data = NULL;
	data->chunk->offset = 0;
	data->chunk->size = buffer->size;
	data->chunk->stride = buffer->pitch;
}

static void on_stream_add_buffer(void *data, struct pw_buffer *pipewire_buffer)
{
	struct pw_castkms *bridge = data;
	struct capture_buffer *buffer;
	struct spa_buffer *spa_buffer;
	int status;

	if (bridge->buffer_count >= PW_CASTKMS_BUFFER_LIMIT ||
	    bridge->buffer_count >= bridge->max_registered_buffers) {
		pw_castkms_fail(bridge, "PipeWire supplied too many buffers",
				 -EOVERFLOW);
		return;
	}

	buffer = &bridge->buffers[bridge->buffer_count];
	spa_buffer = pipewire_buffer->buffer;
	if (!spa_buffer || !spa_buffer->n_datas ||
	    !spa_buffer->datas[0].chunk) {
		pw_castkms_fail(bridge, "PipeWire supplied an invalid buffer",
				 -EPROTO);
		return;
	}

	if (spa_buffer->n_datas != 1) {
		pw_castkms_fail(bridge, "PipeWire buffer layout is invalid",
				 -EPROTO);
		return;
	}
	if (bridge->restart_capture_on_buffer_add) {
		if (bridge->buffer_count) {
			pw_castkms_fail(
				bridge,
				"PipeWire replaced only part of the capture pool",
				-EPROTO);
			return;
		}
		status = castkms_start_capture(bridge);
		if (status) {
			pw_castkms_fail(bridge,
					"could not restart capture for PipeWire",
					status);
			return;
		}
		bridge->restart_capture_on_buffer_add = false;
	}

	status = castkms_create_destination(bridge, buffer);
	if (status < 0) {
		pw_castkms_fail(bridge, "capture buffer allocation failed",
				 status);
		return;
	}

	buffer->pipewire_buffer = pipewire_buffer;
	buffer->state = CAPTURE_BUFFER_IN_PIPEWIRE;
	configure_dmabuf_data(&spa_buffer->datas[0], buffer);

	bridge->buffer_count++;
}

static void on_stream_remove_buffer(void *data,
				    struct pw_buffer *pipewire_buffer)
{
	struct pw_castkms *bridge = data;
	struct capture_buffer *buffer = castkms_find_buffer_by_pipewire(
		bridge, pipewire_buffer);
	uint32_t index;
	int status;

	if (!buffer)
		return;

	/*
	 * PipeWire removes the complete pool when its last consumer pauses.
	 * STOP synchronously cancels queued captures and invalidates every old
	 * registration; a later add_buffer callback starts a fresh stream before
	 * creating the replacement pool.
	 */
	if (bridge->capture_active && !bridge->shutting_down) {
		status = castkms_stop_capture(bridge);
		if (status) {
			pw_castkms_fail(
				bridge, "could not stop capture for PipeWire",
				status);
			return;
		}
		bridge->restart_capture_on_buffer_add = true;
	}

	index = (uint32_t)(buffer - bridge->buffers);
	status = castkms_destroy_destination(bridge, buffer);
	if (status) {
		pw_castkms_fail(bridge, "could not release capture destination",
				 status);
		return;
	}
	if (index < bridge->buffer_count - 1)
		bridge->buffers[index] =
			bridge->buffers[bridge->buffer_count - 1];
	bridge->buffer_count--;
}

static int set_frame_metadata(struct pw_castkms *bridge,
			      struct capture_buffer *buffer,
			      struct spa_buffer *spa_buffer)
{
	const struct captured_frame *frame = &buffer->frame;
	struct spa_meta_header *header;
	struct spa_meta *damage_meta;

	(void)bridge;

	header = spa_buffer_find_meta_data(spa_buffer, SPA_META_Header,
					   sizeof(*header));
	if (header) {
		header->pts = frame->timestamp_ns;
		header->dts_offset = 0;
		header->seq = frame->sequence;
		header->flags = 0;
	}

	damage_meta = spa_buffer_find_meta(spa_buffer, SPA_META_VideoDamage);
	if (damage_meta) {
		struct spa_meta_region *region;
		uint32_t index = 0;

		spa_meta_for_each(region, damage_meta) {
			if (!index && frame->damage.width && frame->damage.height) {
				region->region.position.x = frame->damage.x;
				region->region.position.y = frame->damage.y;
				region->region.size.width = frame->damage.width;
				region->region.size.height = frame->damage.height;
			} else {
				region->region = SPA_REGION(0, 0, 0, 0);
			}
			index++;
		}
	}

	return 0;
}

static bool publish_ready_frames(struct pw_castkms *bridge)
{
	uint32_t i;

	for (i = 0; i < bridge->buffer_count; i++) {
		struct capture_buffer *buffer = &bridge->buffers[i];
		struct spa_buffer *spa_buffer;
		int status;

		if (buffer->state != CAPTURE_BUFFER_READY)
			continue;

		spa_buffer = buffer->pipewire_buffer->buffer;
		status = set_frame_metadata(bridge, buffer, spa_buffer);
		if (status) {
			pw_castkms_fail(bridge, "frame metadata failed", status);
			return false;
		}

		spa_buffer->datas[0].chunk->offset = 0;
		spa_buffer->datas[0].chunk->size = buffer->size;
		spa_buffer->datas[0].chunk->stride = buffer->pitch;

		if (pw_stream_queue_buffer(bridge->stream,
					   buffer->pipewire_buffer) < 0) {
			pw_castkms_fail(bridge,
					"pw_stream_queue_buffer failed", 0);
			return false;
		}
		buffer->state = CAPTURE_BUFFER_IN_PIPEWIRE;
		bridge->frames_produced++;
	}

	return true;
}

static bool reclaim_pipewire_buffers(struct pw_castkms *bridge)
{
	struct pw_buffer *pipewire_buffer;

	while ((pipewire_buffer = pw_stream_dequeue_buffer(bridge->stream))) {
		struct capture_buffer *buffer = castkms_find_buffer_by_pipewire(
			bridge, pipewire_buffer);

		if (!buffer || buffer->state != CAPTURE_BUFFER_IN_PIPEWIRE) {
			(void)pw_stream_queue_buffer(bridge->stream, pipewire_buffer);
			pw_castkms_fail(
				bridge, "unexpected PipeWire buffer ownership",
				-EPROTO);
			return false;
		}

		buffer->state = CAPTURE_BUFFER_AVAILABLE;
	}

	return true;
}

static void on_stream_process(void *data)
{
	struct pw_castkms *bridge = data;

	if (!publish_ready_frames(bridge) ||
	    !reclaim_pipewire_buffers(bridge))
		return;

	castkms_queue_available(bridge);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_stream_state_changed,
	.param_changed = on_stream_param_changed,
	.add_buffer = on_stream_add_buffer,
	.remove_buffer = on_stream_remove_buffer,
	.process = on_stream_process,
};

/* ---- PipeWire core and application lifetime --------------------------- */

static void on_core_error(void *data, uint32_t id, int seq,
			  int status, const char *message)
{
	struct pw_castkms *bridge = data;

	(void)seq;
	fprintf(stderr, "core error id=%u: %s (%s)\n",
		id, message, spa_strerror(status));

	if (status < 0 && !bridge->shutting_down)
		pw_castkms_fail(bridge, "PipeWire core error", status);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

static void on_signal(void *data, int signal_number)
{
	struct pw_castkms *bridge = data;

	(void)signal_number;
	if (!bridge->failed)
		bridge->exit_status = EXIT_SUCCESS;
	pw_main_loop_quit(bridge->loop);
}

static struct pw_properties *create_node_properties(
	const struct pw_castkms *bridge, const char *node_name)
{
	char crtc_id[16];

	(void)snprintf(crtc_id, sizeof(crtc_id), "%u", bridge->crtc_id);

	return pw_properties_new(
		PW_KEY_MEDIA_CLASS, "Video/Source",
		PW_KEY_NODE_NAME, node_name,
		PW_KEY_NODE_DESCRIPTION, bridge->connector_name,
		PW_KEY_NODE_EXCLUSIVE, "true",
		"node.reliable", "true",
		"device.api", "drm",
		"api.castkms.card", bridge->card_label,
		"api.castkms.crtc-id", crtc_id,
		"api.castkms.connector", bridge->connector_name,
		NULL);
}

static int connect_stream(struct pw_castkms *bridge)
{
	uint8_t format_buffer[1024];
	struct spa_pod_builder builder;
	const struct spa_pod *params[1];

	spa_pod_builder_init(&builder, format_buffer, sizeof(format_buffer));
	params[0] = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx),
		SPA_FORMAT_VIDEO_modifier, SPA_POD_Long(DRM_FORMAT_MOD_LINEAR),
		SPA_FORMAT_VIDEO_size,
			SPA_POD_Rectangle(
				&SPA_RECTANGLE(bridge->width, bridge->height)),
		SPA_FORMAT_VIDEO_framerate,
			SPA_POD_Fraction(&SPA_FRACTION(bridge->refresh, 1)));

	return pw_stream_connect(
		bridge->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
		PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_ALLOC_BUFFERS |
			PW_STREAM_FLAG_EXCLUSIVE,
		params, 1);
}

int pipewire_open(struct pw_castkms *bridge)
{
	struct pw_properties *properties;
	const char *card;
	char node_name[128];
	int status;

	bridge->loop = pw_main_loop_new(NULL);
	if (!bridge->loop) {
		fprintf(stderr, "pw_main_loop_new failed\n");
		return -ENOMEM;
	}

	bridge->sigint_source = pw_loop_add_signal(
		pw_main_loop_get_loop(bridge->loop), SIGINT, on_signal, bridge);
	bridge->sigterm_source = pw_loop_add_signal(
		pw_main_loop_get_loop(bridge->loop), SIGTERM, on_signal, bridge);
	if (!bridge->sigint_source || !bridge->sigterm_source) {
		fprintf(stderr, "pw_loop_add_signal failed\n");
		return -ENOMEM;
	}

	bridge->context = pw_context_new(
		pw_main_loop_get_loop(bridge->loop), NULL, 0);
	if (!bridge->context) {
		fprintf(stderr, "pw_context_new failed\n");
		return -ENOMEM;
	}

	bridge->core = pw_context_connect(bridge->context, NULL, 0);
	if (!bridge->core) {
		status = errno ? -errno : -EIO;
		fprintf(stderr, "pw_context_connect: %s\n", strerror(-status));
		return status;
	}

	pw_core_add_listener(bridge->core, &bridge->core_listener,
			     &core_events, bridge);

	bridge->drm_source = pw_loop_add_io(
		pw_main_loop_get_loop(bridge->loop), bridge->drm_fd,
		SPA_IO_IN, false, castkms_on_fd_ready, bridge);
	if (!bridge->drm_source) {
		fprintf(stderr, "pw_loop_add_io failed\n");
		return -ENOMEM;
	}

	bridge->process_timer = pw_loop_add_timer(
		pw_main_loop_get_loop(bridge->loop), on_process_timer, bridge);
	if (!bridge->process_timer) {
		fprintf(stderr, "pw_loop_add_timer failed\n");
		return -ENOMEM;
	}

	card = strrchr(bridge->card_label, '/');
	card = card ? card + 1 : bridge->card_label;
	(void)snprintf(node_name, sizeof(node_name),
		       "castkms.%.32s.crtc-%u", card, bridge->crtc_id);
	properties = create_node_properties(bridge, node_name);
	if (!properties) {
		fprintf(stderr, "pw_properties_new failed\n");
		return -ENOMEM;
	}

	bridge->stream = pw_stream_new(bridge->core, node_name, properties);
	if (!bridge->stream) {
		fprintf(stderr, "pw_stream_new failed\n");
		return -ENOMEM;
	}

	pw_stream_add_listener(bridge->stream, &bridge->stream_listener,
			       &stream_events, bridge);
	status = connect_stream(bridge);
	if (status < 0) {
		fprintf(stderr, "pw_stream_connect: %s\n",
			spa_strerror(status));
		return status;
	}

	return 0;
}

int pipewire_run(struct pw_castkms *bridge)
{
	int status;

	fprintf(stderr, "running\n");
	status = pw_main_loop_run(bridge->loop);
	if (status < 0 && !bridge->failed)
		pw_castkms_fail(bridge, "PipeWire main loop failed", status);

	fprintf(stderr, "produced %llu frames\n",
		(unsigned long long)bridge->frames_produced);
	return status;
}

void pipewire_close(struct pw_castkms *bridge)
{
	struct pw_loop *loop;

	bridge->shutting_down = true;
	if (!bridge->loop)
		return;

	loop = pw_main_loop_get_loop(bridge->loop);
	if (bridge->process_timer)
		(void)update_process_timer(bridge, false);

	if (bridge->stream)
		pw_stream_destroy(bridge->stream);
	bridge->stream = NULL;

	if (bridge->process_timer)
		pw_loop_destroy_source(loop, bridge->process_timer);
	bridge->process_timer = NULL;
	if (bridge->drm_source)
		pw_loop_destroy_source(loop, bridge->drm_source);
	bridge->drm_source = NULL;
	if (bridge->sigterm_source)
		pw_loop_destroy_source(loop, bridge->sigterm_source);
	bridge->sigterm_source = NULL;
	if (bridge->sigint_source)
		pw_loop_destroy_source(loop, bridge->sigint_source);
	bridge->sigint_source = NULL;

	if (bridge->core)
		pw_core_disconnect(bridge->core);
	bridge->core = NULL;
	if (bridge->context)
		pw_context_destroy(bridge->context);
	bridge->context = NULL;
	pw_main_loop_destroy(bridge->loop);
	bridge->loop = NULL;
}
