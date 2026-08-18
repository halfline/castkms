// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <drm/drm_fourcc.h>

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#define DEFAULT_FRAME_COUNT	30
#define DEFAULT_TIMEOUT_SEC	10

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
	int n = 0;

	if (!param || id != SPA_PARAM_Format)
		return;

	spa_format_video_raw_parse(param, &info);
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

	pw_stream_update_params(s->stream, params, n);
}

static void on_process(void *data)
{
	struct test_state *s = data;
	struct pw_buffer *buf;
	struct spa_buffer *spa_buf;
	struct spa_meta_header *h;

	buf = pw_stream_dequeue_buffer(s->stream);
	if (!buf)
		return;

	spa_buf = buf->buffer;

	h = spa_buffer_find_meta_data(spa_buf, SPA_META_Header, sizeof(*h));
	if (!h) {
		s->meta_errors++;
	} else {
		if (s->frames_received > 0) {
			if (h->seq <= s->last_seq)
				s->seq_errors++;
			if (h->pts <= s->last_pts)
				s->pts_errors++;
		}
		s->last_seq = h->seq;
		s->last_pts = h->pts;
	}

	if (spa_buf->n_datas < 1 || !spa_buf->datas[0].chunk ||
	    !spa_buf->datas[0].chunk->size)
		s->data_errors++;

	s->frames_received++;
	pw_stream_queue_buffer(s->stream, buf);

	if (s->frames_received >= s->target_frames)
		pw_main_loop_quit(s->loop);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed,
	.param_changed = on_param_changed,
	.process = on_process,
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

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [-n node-name] [-f frame-count] [-t timeout-sec]\n",
		prog);
}

int main(int argc, char *argv[])
{
	struct test_state state = {};
	struct test_state *s = &state;
	uint8_t format_buf[1024];
	struct spa_pod_builder builder;
	const struct spa_pod *params[1];
	struct pw_properties *props;
	const char *node_name = NULL;
	struct timespec timeout_val;
	int timeout_sec = DEFAULT_TIMEOUT_SEC;
	bool passed;
	int opt;
	int ret = EXIT_FAILURE;

	s->target_frames = DEFAULT_FRAME_COUNT;

	while ((opt = getopt(argc, argv, "n:f:t:h")) != -1) {
		switch (opt) {
		case 'n':
			node_name = optarg;
			break;
		case 'f':
			s->target_frames = atoi(optarg);
			break;
		case 't':
			timeout_sec = atoi(optarg);
			break;
		case 'h':
		default:
			usage(argv[0]);
			return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	pw_init(&argc, &argv);

	s->loop = pw_main_loop_new(NULL);
	if (!s->loop) {
		fprintf(stderr, "pw_main_loop_new failed\n");
		goto out_deinit;
	}

	s->ctx = pw_context_new(pw_main_loop_get_loop(s->loop), NULL, 0);
	if (!s->ctx) {
		fprintf(stderr, "pw_context_new failed\n");
		goto out_loop;
	}

	s->core = pw_context_connect(s->ctx, NULL, 0);
	if (!s->core) {
		fprintf(stderr, "pw_context_connect: %s\n", strerror(errno));
		goto out_ctx;
	}

	pw_core_add_listener(s->core, &s->core_listener, &core_events, s);

	s->timer = pw_loop_add_timer(pw_main_loop_get_loop(s->loop),
				      on_timeout, s);
	timeout_val = (struct timespec){ .tv_sec = timeout_sec };
	pw_loop_update_timer(pw_main_loop_get_loop(s->loop), s->timer,
			      &timeout_val, NULL, false);

	props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Video",
		PW_KEY_MEDIA_CATEGORY, "Capture",
		PW_KEY_MEDIA_ROLE, "Screen",
		NULL);
	if (node_name)
		pw_properties_set(props, PW_KEY_TARGET_OBJECT, node_name);

	s->stream = pw_stream_new(s->core, "pw-castkms-test", props);
	if (!s->stream) {
		fprintf(stderr, "pw_stream_new failed\n");
		goto out_timer;
	}

	pw_stream_add_listener(s->stream, &s->stream_listener,
			       &stream_events, s);

	spa_pod_builder_init(&builder, format_buf, sizeof(format_buf));
	params[0] = spa_pod_builder_add_object(&builder,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType,
			SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype,
			SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format,
			SPA_POD_CHOICE_ENUM_Id(2,
				SPA_VIDEO_FORMAT_BGRx,
				SPA_VIDEO_FORMAT_BGRx),
		SPA_FORMAT_VIDEO_modifier,
			SPA_POD_Long(DRM_FORMAT_MOD_LINEAR),
		SPA_FORMAT_VIDEO_size,
			SPA_POD_CHOICE_RANGE_Rectangle(
				&SPA_RECTANGLE(1920, 1080),
				&SPA_RECTANGLE(1, 1),
				&SPA_RECTANGLE(8192, 8192)),
		SPA_FORMAT_VIDEO_framerate,
			SPA_POD_CHOICE_RANGE_Fraction(
				&SPA_FRACTION(60, 1),
				&SPA_FRACTION(1, 1),
				&SPA_FRACTION(240, 1)));

	if (pw_stream_connect(s->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
			      PW_STREAM_FLAG_AUTOCONNECT, params, 1) < 0) {
		fprintf(stderr, "pw_stream_connect failed\n");
		goto out_stream;
	}

	pw_main_loop_run(s->loop);

	printf("pw_connected=1\n");
	printf("format_negotiated=%d\n", s->format_negotiated ? 1 : 0);
	if (s->format_negotiated) {
		printf("width=%u\n", s->width);
		printf("height=%u\n", s->height);
		printf("framerate=%u/%u\n",
		       s->framerate_num, s->framerate_denom);
	}
	printf("frames_received=%d\n", s->frames_received);
	printf("target_frames=%d\n", s->target_frames);
	printf("timed_out=%d\n", s->timed_out ? 1 : 0);
	printf("sequence_monotonic=%d\n", s->seq_errors == 0 ? 1 : 0);
	printf("timestamp_monotonic=%d\n", s->pts_errors == 0 ? 1 : 0);
	printf("meta_present=%d\n", s->meta_errors == 0 ? 1 : 0);
	printf("data_valid=%d\n", s->data_errors == 0 ? 1 : 0);

	passed = s->format_negotiated &&
		 s->frames_received >= s->target_frames &&
		 !s->timed_out &&
		 !s->seq_errors &&
		 !s->pts_errors &&
		 !s->meta_errors &&
		 !s->data_errors;
	printf("pw_castkms_test=%s\n", passed ? "pass" : "fail");

	ret = passed ? EXIT_SUCCESS : EXIT_FAILURE;

out_stream:
	pw_stream_destroy(s->stream);
out_timer:
	pw_loop_destroy_source(pw_main_loop_get_loop(s->loop), s->timer);
	pw_core_disconnect(s->core);
out_ctx:
	pw_context_destroy(s->ctx);
out_loop:
	pw_main_loop_destroy(s->loop);
out_deinit:
	pw_deinit();
	return ret;
}
