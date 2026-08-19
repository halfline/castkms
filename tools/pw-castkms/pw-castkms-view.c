// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm_fourcc.h>

#include <glib-unix.h>
#include <gtk/gtk.h>

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

struct viewer {
	GtkWindow *window;
	GtkPicture *picture;

	struct pw_loop *pw_loop;
	struct pw_context *ctx;
	struct pw_core *core;
	struct spa_hook core_listener;
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	guint pw_source_id;

	uint32_t width;
	uint32_t height;

	const char *node_name;
};

static gboolean on_pw_fd(int fd, GIOCondition cond, gpointer data)
{
	struct viewer *v = data;

	(void)fd;
	(void)cond;
	pw_loop_iterate(v->pw_loop, 0);
	return G_SOURCE_CONTINUE;
}

static void on_state_changed(void *data, enum pw_stream_state old,
			      enum pw_stream_state state, const char *error)
{
	(void)data;
	(void)old;
	fprintf(stderr, "stream: %s", pw_stream_state_as_string(state));
	if (error)
		fprintf(stderr, " (%s)", error);
	fprintf(stderr, "\n");
}

static void on_param_changed(void *data, uint32_t id,
			      const struct spa_pod *param)
{
	struct viewer *v = data;
	struct spa_video_info_raw info;
	uint8_t buf[1024];
	struct spa_pod_builder b;
	const struct spa_pod *params[2];
	int n = 0;
	char title[128];

	if (!param || id != SPA_PARAM_Format)
		return;

	spa_format_video_raw_parse(param, &info);
	v->width = info.size.width;
	v->height = info.size.height;

	snprintf(title, sizeof(title), "castkms \xe2\x80\x94 %u\xc3\x97%u",
		 v->width, v->height);
	gtk_window_set_title(v->window, title);
	gtk_window_set_default_size(v->window, v->width, v->height);

	spa_pod_builder_init(&b, buf, sizeof(buf));
	params[n++] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers,
			SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_dataType,
			SPA_POD_CHOICE_FLAGS_Int(
				(1 << SPA_DATA_DmaBuf) |
				(1 << SPA_DATA_MemFd) |
				(1 << SPA_DATA_MemPtr)));

	params[n++] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size,
			SPA_POD_Int(sizeof(struct spa_meta_header)));

	pw_stream_update_params(v->stream, params, n);
}

static void on_process(void *data)
{
	struct viewer *v = data;
	struct pw_buffer *pw_buf;
	struct spa_buffer *spa_buf;
	struct spa_data *d;
	void *pixels;
	void *map = MAP_FAILED;
	uint32_t stride;
	size_t size;
	GBytes *bytes;
	GdkTexture *texture;

	pw_buf = pw_stream_dequeue_buffer(v->stream);
	if (!pw_buf)
		return;

	spa_buf = pw_buf->buffer;
	if (spa_buf->n_datas < 1 || !v->width || !v->height)
		goto done;

	d = &spa_buf->datas[0];
	if (!d->chunk || !d->chunk->size)
		goto done;

	stride = d->chunk->stride;
	if (!stride)
		goto done;

	size = (size_t)stride * v->height;
	if (d->chunk->size < size)
		goto done;

	switch (d->type) {
	case SPA_DATA_DmaBuf:
	case SPA_DATA_MemFd:
		map = mmap(NULL, d->chunk->offset + size, PROT_READ,
			   MAP_SHARED, d->fd, d->mapoffset);
		if (map == MAP_FAILED)
			goto done;
		pixels = (char *)map + d->chunk->offset;
		break;
	case SPA_DATA_MemPtr:
		if (!d->data)
			goto done;
		pixels = (char *)d->data + d->mapoffset + d->chunk->offset;
		break;
	default:
		goto done;
	}

	bytes = g_bytes_new(pixels, size);
	if (map != MAP_FAILED)
		munmap(map, d->chunk->offset + size);

	texture = gdk_memory_texture_new(v->width, v->height,
					 GDK_MEMORY_B8G8R8X8, bytes, stride);
	g_bytes_unref(bytes);

	gtk_picture_set_paintable(v->picture, GDK_PAINTABLE(texture));
	g_object_unref(texture);

done:
	pw_stream_queue_buffer(v->stream, pw_buf);
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
	(void)data;
	(void)seq;
	fprintf(stderr, "core error id=%u: %s (%s)\n",
		id, message, spa_strerror(res));
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

static void start_pipewire(struct viewer *v)
{
	uint8_t buf[1024];
	struct spa_pod_builder b;
	const struct spa_pod *params[1];
	struct pw_properties *props;
	int pw_fd;

	v->pw_loop = pw_loop_new(NULL);
	if (!v->pw_loop) {
		fprintf(stderr, "pw_loop_new failed\n");
		return;
	}

	v->ctx = pw_context_new(v->pw_loop, NULL, 0);
	if (!v->ctx) {
		fprintf(stderr, "pw_context_new failed\n");
		return;
	}

	v->core = pw_context_connect(v->ctx, NULL, 0);
	if (!v->core) {
		fprintf(stderr, "pw_context_connect: %s\n", strerror(errno));
		return;
	}

	pw_core_add_listener(v->core, &v->core_listener, &core_events, v);

	props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Video",
		PW_KEY_MEDIA_CATEGORY, "Capture",
		PW_KEY_MEDIA_ROLE, "Screen",
		NULL);
	if (v->node_name)
		pw_properties_set(props, PW_KEY_TARGET_OBJECT, v->node_name);

	v->stream = pw_stream_new(v->core, "pw-castkms-view", props);
	pw_stream_add_listener(v->stream, &v->stream_listener,
			       &stream_events, v);

	spa_pod_builder_init(&b, buf, sizeof(buf));
	params[0] = spa_pod_builder_add_object(&b,
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

	pw_stream_connect(v->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
			  PW_STREAM_FLAG_AUTOCONNECT, params, 1);

	pw_fd = pw_loop_get_fd(v->pw_loop);
	v->pw_source_id = g_unix_fd_add(pw_fd, G_IO_IN | G_IO_ERR,
					 on_pw_fd, v);
}

static void stop_pipewire(struct viewer *v)
{
	if (v->pw_source_id) {
		g_source_remove(v->pw_source_id);
		v->pw_source_id = 0;
	}
	if (v->stream)
		pw_stream_destroy(v->stream);
	if (v->core)
		pw_core_disconnect(v->core);
	if (v->ctx)
		pw_context_destroy(v->ctx);
	if (v->pw_loop)
		pw_loop_destroy(v->pw_loop);
}

static void on_activate(GtkApplication *app, gpointer data)
{
	struct viewer *v = data;
	GtkWidget *window, *picture;

	window = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(window), "castkms");
	gtk_window_set_default_size(GTK_WINDOW(window), 640, 480);
	v->window = GTK_WINDOW(window);

	picture = gtk_picture_new();
	gtk_picture_set_content_fit(GTK_PICTURE(picture),
				    GTK_CONTENT_FIT_CONTAIN);
	gtk_window_set_child(v->window, picture);
	v->picture = GTK_PICTURE(picture);

	gtk_window_present(v->window);
	start_pipewire(v);
}

static void on_shutdown(GtkApplication *app, gpointer data)
{
	(void)app;
	stop_pipewire(data);
}

int main(int argc, char *argv[])
{
	struct viewer v = {};
	GtkApplication *app;
	int opt, ret;

	while ((opt = getopt(argc, argv, "n:h")) != -1) {
		switch (opt) {
		case 'n':
			v.node_name = optarg;
			break;
		default:
			fprintf(stderr,
				"Usage: %s [-n node-name]\n", argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}

	pw_init(&argc, &argv);

	app = gtk_application_new("org.castkms.view",
				  G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(app, "activate", G_CALLBACK(on_activate), &v);
	g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), &v);

	ret = g_application_run(G_APPLICATION(app), 0, NULL);
	g_object_unref(app);
	pw_deinit();
	return ret;
}
