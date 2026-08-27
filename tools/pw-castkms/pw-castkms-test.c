// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <drm/drm_fourcc.h>

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#define DEFAULT_FRAME_COUNT	30
#define DEFAULT_TIMEOUT_SEC	10
#define MAX_FRAME_DIMENSION	8192U

struct test_state {
	struct pw_main_loop *loop;
	struct pw_context *ctx;
	struct pw_core *core;
	struct spa_hook core_listener;
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct spa_source *timer;

	uint32_t width;
	uint32_t height;
	uint32_t framerate_num;
	uint32_t framerate_denom;
	bool format_negotiated;

	int target_frames;
	int frames_received;
	uint64_t last_seq;
	int64_t last_pts;
	int seq_errors;
	int pts_errors;
	int meta_errors;
	int data_errors;
	int format_errors;

	bool timed_out;
	bool streaming;
};

static void on_timeout(void *data, uint64_t expirations)
{
	struct test_state *s = data;

	(void)expirations;
	s->timed_out = true;
	pw_main_loop_quit(s->loop);
}

static void on_state_changed(void *data, enum pw_stream_state old,
			      enum pw_stream_state state, const char *error)
{
	struct test_state *s = data;

	(void)old;
	fprintf(stderr, "test stream: %s", pw_stream_state_as_string(state));
	if (error)
		fprintf(stderr, " (%s)", error);
	fprintf(stderr, "\n");

	switch (state) {
	case PW_STREAM_STATE_STREAMING:
		s->streaming = true;
		break;
	case PW_STREAM_STATE_ERROR:
		pw_main_loop_quit(s->loop);
		break;
	case PW_STREAM_STATE_UNCONNECTED:
		if (s->streaming)
			pw_main_loop_quit(s->loop);
		break;
	default:
		break;
	}
}

static void on_param_changed(void *data, uint32_t id,
			      const struct spa_pod *param)
{
	struct test_state *s = data;
	struct spa_video_info_raw info;
	uint8_t params_buf[1024];
	struct spa_pod_builder builder;
	const struct spa_pod *params[2];
	uint32_t media_type;
	uint32_t media_subtype;
	int ret;
	int n = 0;

	if (!param || id != SPA_PARAM_Format)
		return;

	memset(&info, 0, sizeof(info));
	ret = spa_format_parse(param, &media_type, &media_subtype);
	if (ret < 0 || media_type != SPA_MEDIA_TYPE_video ||
	    media_subtype != SPA_MEDIA_SUBTYPE_raw ||
	    spa_format_video_raw_parse(param, &info) < 0 ||
	    info.format != SPA_VIDEO_FORMAT_BGRx ||
	    !(info.flags & SPA_VIDEO_FLAG_MODIFIER) ||
	    info.modifier != DRM_FORMAT_MOD_LINEAR ||
	    !info.size.width || !info.size.height ||
	    info.size.width > MAX_FRAME_DIMENSION ||
	    info.size.height > MAX_FRAME_DIMENSION) {
		fprintf(stderr, "unsupported negotiated video format\n");
		s->format_errors++;
		pw_main_loop_quit(s->loop);
		return;
	}

	s->width = info.size.width;
	s->height = info.size.height;
	s->framerate_num = info.framerate.num;
	s->framerate_denom = info.framerate.denom;
	s->format_negotiated = true;

	spa_pod_builder_init(&builder, params_buf, sizeof(params_buf));

	params[n++] = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers,
			SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_dataType,
			SPA_POD_CHOICE_FLAGS_Int(
				(1 << SPA_DATA_DmaBuf) |
				(1 << SPA_DATA_MemFd) |
				(1 << SPA_DATA_MemPtr)));

	params[n++] = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size,
			SPA_POD_Int(sizeof(struct spa_meta_header)));

	ret = pw_stream_update_params(s->stream, params, n);
	if (ret < 0) {
		fprintf(stderr, "pw_stream_update_params: %s\n",
			spa_strerror(ret));
		s->format_errors++;
		pw_main_loop_quit(s->loop);
	}
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed,
	.param_changed = on_param_changed,
};

static void on_core_error(void *data, uint32_t id, int seq,
			   int res, const char *message)
{
	struct test_state *s = data;

	(void)seq;
	fprintf(stderr, "core error id=%u: %s (%s)\n",
		id, message, spa_strerror(res));

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_main_loop_quit(s->loop);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};
