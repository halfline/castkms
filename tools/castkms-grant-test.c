// SPDX-License-Identifier: GPL-2.0-only

#include <drm/castkms_drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static_assert(sizeof(struct drm_castkms_create_grant) == 24,
	      "create-grant ABI size changed");
static_assert(sizeof(struct drm_castkms_revoke_grant) == 16,
	      "revoke-grant ABI size changed");
static_assert(sizeof(struct drm_castkms_get_grant) == 32,
	      "get-grant ABI size changed");
static_assert(sizeof(struct drm_event_castkms_grant_revoked) == 24,
	      "grant-revoked event ABI size changed");
static_assert(sizeof(struct drm_event_castkms_grant_state) == 32,
	      "grant-state event ABI size changed");

struct test_framebuffer {
	uint32_t handle;
	uint32_t fb_id;
};

struct test_display {
	struct drm_mode_modeinfo mode;
	struct test_framebuffer source;
	uint32_t crtc_id;
};

static int parse_connector_id(const char *value, uint32_t *connector_id)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(value, &end, 0);
	if (errno || !*value || *end || !parsed || parsed > UINT32_MAX)
		return -1;
	*connector_id = parsed;
	return 0;
}

static int expect_ioctl_errno(int fd, unsigned long request, void *arg,
			      int expected, const char *operation)
{
	if (ioctl(fd, request, arg) == 0) {
		fprintf(stderr, "%s unexpectedly succeeded\n", operation);
		return -1;
	}
	if (errno != expected) {
		fprintf(stderr, "%s returned %s, expected %s\n", operation,
			strerror(errno), strerror(expected));
		return -1;
	}
	return 0;
}

static int create_grant(int issuer_fd, uint32_t connector_id, uint32_t rights,
			uint32_t flags, int *grant_fd, uint32_t *grant_id)
{
	struct drm_castkms_create_grant create = {
		.connector_id = connector_id,
		.rights = rights,
		.flags = flags,
		.fd_flags = O_NONBLOCK,
	};

	if (ioctl(issuer_fd, DRM_IOCTL_CASTKMS_CREATE_GRANT, &create) < 0) {
		perror("DRM_IOCTL_CASTKMS_CREATE_GRANT");
		return -1;
	}
	if (create.fd < 0 || !create.grant_id) {
		fprintf(stderr, "create grant returned invalid outputs\n");
		if (create.fd >= 0)
			close(create.fd);
		return -1;
	}
	if (!(fcntl(create.fd, F_GETFD) & FD_CLOEXEC)) {
		fprintf(stderr, "grant fd is missing FD_CLOEXEC\n");
		close(create.fd);
		return -1;
	}

	*grant_fd = create.fd;
	*grant_id = create.grant_id;
	return 0;
}

static int expect_create_grant_errno(int issuer_fd, uint32_t connector_id,
				     uint32_t flags, int expected,
				     const char *operation)
{
	struct drm_castkms_create_grant create = {
		.connector_id = connector_id,
		.rights = DRM_CASTKMS_GRANT_CAPTURE_PIXELS,
		.flags = flags,
	};

	if (ioctl(issuer_fd, DRM_IOCTL_CASTKMS_CREATE_GRANT, &create) == 0) {
		fprintf(stderr, "%s unexpectedly succeeded\n", operation);
		if (create.fd >= 0)
			close(create.fd);
		return -1;
	}
	if (errno != expected) {
		fprintf(stderr, "%s returned %s, expected %s\n", operation,
			strerror(errno), strerror(expected));
		return -1;
	}

	return 0;
}

static int create_test_framebuffer(int fd, uint32_t width, uint32_t height,
				   struct test_framebuffer *buffer)
{
	struct drm_mode_create_dumb dumb = {
		.width = width,
		.height = height,
		.bpp = 32,
	};
	struct drm_mode_fb_cmd2 fb = {
		.width = width,
		.height = height,
		.pixel_format = DRM_FORMAT_XRGB8888,
	};
	struct drm_mode_destroy_dumb destroy;

	*buffer = (struct test_framebuffer) {};
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) < 0) {
		perror("DRM_IOCTL_MODE_CREATE_DUMB");
		return -1;
	}

	fb.handles[0] = dumb.handle;
	fb.pitches[0] = dumb.pitch;
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0) {
		perror("DRM_IOCTL_MODE_ADDFB2");
		destroy = (struct drm_mode_destroy_dumb) {
			.handle = dumb.handle,
		};
		ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
		return -1;
	}

	buffer->handle = dumb.handle;
	buffer->fb_id = fb.fb_id;
	return 0;
}

static void destroy_test_framebuffer(int fd, struct test_framebuffer *buffer)
{
	struct drm_mode_destroy_dumb destroy = {
		.handle = buffer->handle,
	};

	if (fd < 0)
		return;
	if (buffer->fb_id && ioctl(fd, DRM_IOCTL_MODE_RMFB, &buffer->fb_id) < 0)
		perror("DRM_IOCTL_MODE_RMFB");
	if (buffer->handle &&
	    ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) < 0)
		perror("DRM_IOCTL_MODE_DESTROY_DUMB");
	*buffer = (struct test_framebuffer) {};
}

static int find_connector_mode(int fd, uint32_t connector_id,
			       struct drm_mode_modeinfo *mode,
			       uint32_t *crtc_id)
{
	uint32_t crtc_ids[32];
	uint32_t encoder_ids[32];
	struct drm_mode_modeinfo modes[64];
	struct drm_mode_card_res resources = {
		.count_crtcs = 32,
		.crtc_id_ptr = (uint64_t)(uintptr_t)crtc_ids,
	};
	struct drm_mode_get_connector connector = {
		.connector_id = connector_id,
	};
	uint32_t i;

	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) < 0 ||
	    !resources.count_crtcs || resources.count_crtcs > 32) {
		perror("DRM_IOCTL_MODE_GETRESOURCES");
		return -1;
	}
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) < 0 ||
	    !connector.count_modes || connector.count_modes > 64 ||
	    !connector.count_encoders || connector.count_encoders > 32) {
		perror("DRM_IOCTL_MODE_GETCONNECTOR count");
		return -1;
	}
	connector.modes_ptr = (uint64_t)(uintptr_t)modes;
	connector.encoders_ptr = (uint64_t)(uintptr_t)encoder_ids;
	connector.count_props = 0;
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) < 0) {
		perror("DRM_IOCTL_MODE_GETCONNECTOR values");
		return -1;
	}

	for (i = 0; i < connector.count_encoders; i++) {
		struct drm_mode_get_encoder encoder = {
			.encoder_id = encoder_ids[i],
		};
		uint32_t crtc_index;

		if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &encoder) < 0) {
			perror("DRM_IOCTL_MODE_GETENCODER");
			return -1;
		}
		for (crtc_index = 0; crtc_index < resources.count_crtcs;
		     crtc_index++) {
			if (!(encoder.possible_crtcs & (1U << crtc_index)))
				continue;
			*mode = modes[0];
			*crtc_id = crtc_ids[crtc_index];
			return 0;
		}
	}

	fprintf(stderr, "connector has no usable CRTC\n");
	return -1;
}

static int set_display_framebuffer(int fd, uint32_t connector_id,
				   const struct test_display *display,
				   const struct test_framebuffer *buffer)
{
	struct drm_mode_crtc set = {
		.crtc_id = display->crtc_id,
		.fb_id = buffer->fb_id,
		.set_connectors_ptr = (uint64_t)(uintptr_t)&connector_id,
		.count_connectors = 1,
		.mode = display->mode,
		.mode_valid = 1,
	};

	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
		perror("DRM_IOCTL_MODE_SETCRTC");
		return -1;
	}
	return 0;
}

static int setup_display(int issuer_fd, int grant_fd, uint32_t connector_id,
			 struct test_display *display)
{
	struct drm_castkms_capture_attach_monitor attach = {
		.connector_id = connector_id,
	};

	*display = (struct test_display) {};
	if (ioctl(grant_fd, DRM_IOCTL_CASTKMS_CAPTURE_ATTACH_MONITOR, &attach) < 0) {
		perror("capture setup ATTACH_MONITOR");
		return -1;
	}
	if (find_connector_mode(issuer_fd, connector_id, &display->mode,
				&display->crtc_id) ||
	    create_test_framebuffer(issuer_fd, display->mode.hdisplay,
				    display->mode.vdisplay, &display->source) ||
	    set_display_framebuffer(issuer_fd, connector_id, display,
				    &display->source))
		return -1;

	return 0;
}

static int start_capture(int fd, uint32_t crtc_id,
			 struct drm_castkms_capture_start *start)
{
	*start = (struct drm_castkms_capture_start) {
		.crtc_id = crtc_id,
		.flags = DRM_CASTKMS_CAPTURE_START_EXCLUSIVE |
			 DRM_CASTKMS_CAPTURE_START_EXCLUDE_CURSOR,
	};
	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_START, start) < 0)
		return -errno;
	return 0;
}

static int stop_capture(int fd, uint32_t stream_id)
{
	struct drm_castkms_capture_stop stop = {
		.stream_id = stream_id,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_STOP, &stop) < 0)
		return -errno;
	return 0;
}

static int register_capture_buffer(int fd, uint32_t stream_id,
				   uint32_t fb_id, uint64_t mode_generation,
				   uint32_t *buffer_id)
{
	struct drm_castkms_capture_register_buffer buffer = {
		.stream_id = stream_id,
		.fb_id = fb_id,
		.flags = DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC,
		.mode_generation = mode_generation,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_REGISTER_BUFFER, &buffer) < 0)
		return -errno;
	*buffer_id = buffer.buffer_id;
	return 0;
}

static int queue_capture_buffer(int fd, uint32_t stream_id,
				uint32_t buffer_id, uint64_t mode_generation,
				uint64_t user_data)
{
	struct drm_castkms_capture_queue_buffer buffer = {
		.stream_id = stream_id,
		.buffer_id = buffer_id,
		.flags = DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC,
		.mode_generation = mode_generation,
		.user_data = user_data,
	};

	if (ioctl(fd, DRM_IOCTL_CASTKMS_CAPTURE_QUEUE_BUFFER, &buffer) < 0)
		return -errno;
	return 0;
}

static int expect_capture_start_errno(int fd, uint32_t crtc_id,
				      int expected)
{
	struct drm_castkms_capture_start start;
	int ret = start_capture(fd, crtc_id, &start);

	if (ret != -expected) {
		fprintf(stderr, "CAPTURE_START returned %d, expected -%d\n",
			ret, expected);
		if (!ret)
			stop_capture(fd, start.stream_id);
		return -1;
	}
	return 0;
}

static int get_holder_grant(int grant_fd, struct drm_castkms_get_grant *grant)
{
	*grant = (struct drm_castkms_get_grant) {};
	if (ioctl(grant_fd, DRM_IOCTL_CASTKMS_GET_GRANT, grant) < 0) {
		perror("holder DRM_IOCTL_CASTKMS_GET_GRANT");
		return -1;
	}
	return 0;
}

static int expect_holder_state(int grant_fd, uint32_t grant_id,
			       uint32_t expected_state, uint32_t expected_flags)
{
	struct drm_castkms_get_grant grant;

	if (get_holder_grant(grant_fd, &grant))
		return -1;
	if (grant.grant_id != grant_id || grant.state != expected_state ||
	    grant.flags != expected_flags) {
		fprintf(stderr,
			"grant %u query: id=%u state=%u flags=%#x, expected state=%u flags=%#x\n",
			grant_id, grant.grant_id, grant.state, grant.flags,
			expected_state, expected_flags);
		return -1;
	}
	return 0;
}

static int revoke_grant(int issuer_fd, uint32_t grant_id)
{
	struct drm_castkms_revoke_grant revoke = {
		.grant_id = grant_id,
	};

	if (ioctl(issuer_fd, DRM_IOCTL_CASTKMS_REVOKE_GRANT, &revoke) < 0) {
		perror("DRM_IOCTL_CASTKMS_REVOKE_GRANT");
		return -1;
	}
	return 0;
}

static int expect_revoke_grant_errno(int issuer_fd, uint32_t grant_id,
				     int expected, const char *operation)
{
	struct drm_castkms_revoke_grant revoke = {
		.grant_id = grant_id,
	};

	return expect_ioctl_errno(issuer_fd, DRM_IOCTL_CASTKMS_REVOKE_GRANT,
				  &revoke, expected, operation);
}

static int pass_fd(int source_fd)
{
	char control[CMSG_SPACE(sizeof(int))];
	struct iovec iov;
	struct msghdr message;
	struct cmsghdr *cmsg;
	char byte = 0;
	int sockets[2] = { -1, -1 };
	int received = -1;

	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) < 0) {
		perror("socketpair");
		return -1;
	}

	memset(&message, 0, sizeof(message));
	iov.iov_base = &byte;
	iov.iov_len = sizeof(byte);
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof(control);
	cmsg = CMSG_FIRSTHDR(&message);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(source_fd));
	memcpy(CMSG_DATA(cmsg), &source_fd, sizeof(source_fd));
	if (sendmsg(sockets[0], &message, 0) != sizeof(byte)) {
		perror("sendmsg(SCM_RIGHTS)");
		goto out;
	}

	memset(control, 0, sizeof(control));
	memset(&message, 0, sizeof(message));
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof(control);
	if (recvmsg(sockets[1], &message, MSG_CMSG_CLOEXEC) != sizeof(byte)) {
		perror("recvmsg(SCM_RIGHTS)");
		goto out;
	}
	cmsg = CMSG_FIRSTHDR(&message);
	if (!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
	    cmsg->cmsg_type != SCM_RIGHTS ||
	    cmsg->cmsg_len != CMSG_LEN(sizeof(received))) {
		fprintf(stderr, "invalid SCM_RIGHTS response\n");
		goto out;
	}
	memcpy(&received, CMSG_DATA(cmsg), sizeof(received));

out:
	close(sockets[0]);
	close(sockets[1]);
	return received;
}

static int expect_revoke_event(int grant_fd, uint32_t grant_id, int status)
{
	struct pollfd pollfd = {
		.fd = grant_fd,
		.events = POLLIN,
	};
	unsigned char events[4096];
	int attempts;

	for (attempts = 0; attempts < 20; attempts++) {
		ssize_t size;
		size_t offset;
		int ret;

		ret = poll(&pollfd, 1, 250);
		if (ret < 0) {
			perror("poll grant event");
			return -1;
		}
		if (!ret)
			continue;
		size = read(grant_fd, events, sizeof(events));
		if (size < 0) {
			if (errno == EAGAIN)
				continue;
			perror("read grant event");
			return -1;
		}
		for (offset = 0; offset + sizeof(struct drm_event) <= (size_t)size;) {
			const struct drm_event *base = (const void *)(events + offset);

			if (base->length < sizeof(*base) ||
			    offset + base->length > (size_t)size) {
				fprintf(stderr, "malformed DRM event\n");
				return -1;
			}
			if (base->type == DRM_CASTKMS_CAPTURE_EVENT_GRANT_REVOKED &&
			    base->length ==
				sizeof(struct drm_event_castkms_grant_revoked)) {
				const struct drm_event_castkms_grant_revoked *event =
					(const void *)base;

				if (event->grant_id != grant_id ||
				    event->status != status) {
					fprintf(stderr,
						"unexpected revoke event id=%u status=%d\n",
						event->grant_id, event->status);
					return -1;
				}
				return 0;
			}
			offset += base->length;
		}
	}

	fprintf(stderr, "timed out waiting for grant %u revocation\n", grant_id);
	return -1;
}

static int expect_capture_frame(int grant_fd, uint32_t stream_id,
				uint32_t buffer_id, uint64_t user_data)
{
	struct pollfd pollfd = {
		.fd = grant_fd,
		.events = POLLIN,
	};
	unsigned char events[4096];
	int attempts;

	for (attempts = 0; attempts < 20; attempts++) {
		ssize_t size;
		size_t offset;
		int ret;

		ret = poll(&pollfd, 1, 250);
		if (ret < 0) {
			perror("poll capture event");
			return -1;
		}
		if (!ret)
			continue;
		size = read(grant_fd, events, sizeof(events));
		if (size < 0) {
			if (errno == EAGAIN)
				continue;
			perror("read capture event");
			return -1;
		}
		for (offset = 0; offset + sizeof(struct drm_event) <= (size_t)size;) {
			const struct drm_event *base = (const void *)(events + offset);

			if (base->length < sizeof(*base) ||
			    offset + base->length > (size_t)size) {
				fprintf(stderr, "malformed DRM event\n");
				return -1;
			}
			if (base->type == DRM_CASTKMS_CAPTURE_EVENT_FRAME &&
			    base->length == sizeof(struct drm_event_castkms_capture_frame)) {
				const struct drm_event_castkms_capture_frame *event =
					(const void *)base;

				if (event->stream_id != stream_id ||
				    event->buffer_id != buffer_id ||
				    event->user_data != user_data) {
					offset += base->length;
					continue;
				}
				if (event->status) {
					fprintf(stderr,
						"unexpected capture event stream=%u buffer=%u status=%d\n",
						event->stream_id, event->buffer_id,
						event->status);
					return -1;
				}
				return 0;
			}
			offset += base->length;
		}
	}

	fprintf(stderr, "timed out waiting for a captured frame\n");
	return -1;
}

static int test_capture_frame(int grant_fd, const struct test_display *display)
{
	const uint64_t user_data = UINT64_C(0x4752414e5446524d);
	struct drm_castkms_capture_start stream;
	struct test_framebuffer destination = {};
	uint32_t buffer_id = 0;
	int ioctl_ret;
	int ret = -1;

	ioctl_ret = start_capture(grant_fd, display->crtc_id, &stream);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("grant CAPTURE_START");
		return -1;
	}
	if (create_test_framebuffer(grant_fd, display->mode.hdisplay,
				    display->mode.vdisplay, &destination))
		goto out_stop;
	ioctl_ret = register_capture_buffer(grant_fd, stream.stream_id,
					    destination.fb_id,
					    stream.mode_generation, &buffer_id);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("grant REGISTER_BUFFER");
		goto out_stop;
	}
	ioctl_ret = queue_capture_buffer(grant_fd, stream.stream_id, buffer_id,
					 stream.mode_generation, user_data);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("grant QUEUE_BUFFER");
		goto out_stop;
	}
	if (expect_capture_frame(grant_fd, stream.stream_id, buffer_id,
				 user_data))
		goto out_stop;
	ret = 0;

out_stop:
	ioctl_ret = stop_capture(grant_fd, stream.stream_id);
	if (ioctl_ret && ioctl_ret != -ENOENT) {
		errno = -ioctl_ret;
		perror("grant CAPTURE_STOP");
		ret = -1;
	}
	destroy_test_framebuffer(grant_fd, &destination);
	return ret;
}

static int prepare_capture_stream(
	int grant_fd, const struct test_display *display,
	struct drm_castkms_capture_start *stream,
	struct test_framebuffer *destination, uint32_t *buffer_id)
{
	int ioctl_ret;

	ioctl_ret = start_capture(grant_fd, display->crtc_id, stream);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("grant CAPTURE_START");
		return -1;
	}
	if (create_test_framebuffer(grant_fd, display->mode.hdisplay,
				    display->mode.vdisplay, destination))
		goto out_stop;
	ioctl_ret = register_capture_buffer(
		grant_fd, stream->stream_id, destination->fb_id,
		stream->mode_generation, buffer_id);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("grant REGISTER_BUFFER");
		goto out_destroy;
	}

	return 0;

out_destroy:
	destroy_test_framebuffer(grant_fd, destination);
out_stop:
	stop_capture(grant_fd, stream->stream_id);
	return -1;
}

static int capture_prepared_stream(
	int grant_fd, const struct drm_castkms_capture_start *stream,
	uint32_t buffer_id, uint64_t user_data)
{
	int ioctl_ret;

	ioctl_ret = queue_capture_buffer(
		grant_fd, stream->stream_id, buffer_id,
		stream->mode_generation, user_data);
	if (ioctl_ret) {
		errno = -ioctl_ret;
		perror("grant QUEUE_BUFFER");
		return -1;
	}

	return expect_capture_frame(
		grant_fd, stream->stream_id, buffer_id, user_data);
}

static int expect_capture_stream_collected(
	int grant_fd, const struct drm_castkms_capture_start *stream,
	uint32_t buffer_id)
{
	const uint64_t user_data = UINT64_C(0x5354414c4541444d);
	unsigned int attempt;

	for (attempt = 0; attempt < 100; attempt++) {
		int ioctl_ret = queue_capture_buffer(
			grant_fd, stream->stream_id, buffer_id,
			stream->mode_generation, user_data);

		if (ioctl_ret == -ENOENT)
			return 0;
		if (ioctl_ret != -EAGAIN) {
			fprintf(stderr,
				"stale grant QUEUE_BUFFER returned %d, expected %d or %d\n",
				ioctl_ret, -EAGAIN, -ENOENT);
			return -1;
		}
		usleep(20000);
	}

	fprintf(stderr, "timed out waiting for stale grant stream cleanup\n");
	return -1;
}

static int test_master_cleanup_generation(int issuer_fd, int grant_fd,
					  const struct test_display *display)
{
	struct test_framebuffer destination = {};
	unsigned int iteration;
	int ret = -1;

	if (create_test_framebuffer(grant_fd, display->mode.hdisplay,
				    display->mode.vdisplay, &destination))
		return -1;

	for (iteration = 0; iteration < 32; iteration++) {
		struct drm_castkms_capture_start stream;
		uint32_t buffer_id = 0;
		int ioctl_ret;

		if (ioctl(issuer_fd, DRM_IOCTL_DROP_MASTER, 0) < 0 ||
		    ioctl(issuer_fd, DRM_IOCTL_SET_MASTER, 0) < 0) {
			perror("drop/reacquire master during capture stress");
			goto out;
		}
		ioctl_ret = start_capture(grant_fd, display->crtc_id, &stream);
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("post-reacquire CAPTURE_START");
			goto out;
		}
		ioctl_ret = register_capture_buffer(
			grant_fd, stream.stream_id, destination.fb_id,
			stream.mode_generation, &buffer_id);
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("post-reacquire REGISTER_BUFFER");
			goto out_stop;
		}

		usleep(20000);
		ioctl_ret = queue_capture_buffer(
			grant_fd, stream.stream_id, buffer_id,
			stream.mode_generation, iteration + 1);
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("post-reacquire QUEUE_BUFFER");
			goto out_stop;
		}
		if (expect_capture_frame(grant_fd, stream.stream_id, buffer_id,
					 iteration + 1))
			goto out_stop;
		ioctl_ret = stop_capture(grant_fd, stream.stream_id);
		if (ioctl_ret) {
			errno = -ioctl_ret;
			perror("post-reacquire CAPTURE_STOP");
			goto out;
		}
		continue;

out_stop:
		stop_capture(grant_fd, stream.stream_id);
		goto out;
	}

	ret = 0;
out:
	destroy_test_framebuffer(grant_fd, &destination);
	return ret;
}

static int test_crc_access(int drm_fd, int expected_errno)
{
	struct stat statbuf;
	char control_path[128];
	char data_path[128];
	static const char source[] = "auto\n";
	int control_fd;
	int data_fd;
	int ret = -1;

	if (fstat(drm_fd, &statbuf) < 0) {
		perror("fstat DRM device");
		return -1;
	}
	if (snprintf(control_path, sizeof(control_path),
		     "/sys/kernel/debug/dri/%u/crtc-0/crc/control",
		     minor(statbuf.st_rdev)) >= (int)sizeof(control_path) ||
	    snprintf(data_path, sizeof(data_path),
		     "/sys/kernel/debug/dri/%u/crtc-0/crc/data",
		     minor(statbuf.st_rdev)) >= (int)sizeof(data_path))
		return -1;
	control_fd = open(control_path, O_WRONLY | O_CLOEXEC);
	if (control_fd < 0) {
		perror(control_path);
		return -1;
	}
	if (write(control_fd, source, sizeof(source) - 1) != sizeof(source) - 1) {
		perror("write CRC source");
		close(control_fd);
		return -1;
	}
	close(control_fd);

	errno = 0;
	data_fd = open(data_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (data_fd >= 0) {
		if (!expected_errno)
			ret = 0;
		else
			fprintf(stderr, "CRC data unexpectedly opened\n");
		close(data_fd);
	} else if (expected_errno && errno == expected_errno) {
		ret = 0;
	} else {
		fprintf(stderr, "CRC data open returned %s, expected %s\n",
			strerror(errno),
			expected_errno ? strerror(expected_errno) : "success");
	}
	return ret;
}

static int find_writeback_connector(int fd, uint32_t *connector_id)
{
	uint32_t connector_ids[32];
	struct drm_set_client_cap atomic_cap = {
		.capability = DRM_CLIENT_CAP_ATOMIC,
		.value = 1,
	};
	struct drm_set_client_cap client_cap = {
		.capability = DRM_CLIENT_CAP_WRITEBACK_CONNECTORS,
		.value = 1,
	};
	struct drm_mode_card_res resources = {
		.count_connectors = 32,
		.connector_id_ptr = (uint64_t)(uintptr_t)connector_ids,
	};
	uint32_t i;

	if (ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &atomic_cap) < 0) {
		perror("enable atomic client capability");
		return -1;
	}
	if (ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &client_cap) < 0) {
		perror("enable writeback connector visibility");
		return -1;
	}
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) < 0 ||
	    !resources.count_connectors || resources.count_connectors > 32) {
		perror("GETRESOURCES for writeback");
		return -1;
	}
	for (i = 0; i < resources.count_connectors; i++) {
		struct drm_mode_get_connector connector = {
			.connector_id = connector_ids[i],
		};

		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) < 0) {
			perror("GETCONNECTOR for writeback");
			return -1;
		}
		if (connector.connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
			*connector_id = connector.connector_id;
			return 0;
		}
	}

	fprintf(stderr, "no writeback connector found\n");
	return -1;
}

static int find_connector_property(int fd, uint32_t connector_id,
				   const char *name, uint32_t *property_id)
{
	uint32_t property_ids[64];
	uint64_t property_values[64];
	struct drm_mode_get_connector connector = {
		.connector_id = connector_id,
	};
	uint32_t i;

	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) < 0 ||
	    !connector.count_props || connector.count_props > 64) {
		perror("GETCONNECTOR property count");
		return -1;
	}
	connector.count_modes = 0;
	connector.count_encoders = 0;
	connector.props_ptr = (uint64_t)(uintptr_t)property_ids;
	connector.prop_values_ptr = (uint64_t)(uintptr_t)property_values;
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) < 0) {
		perror("GETCONNECTOR properties");
		return -1;
	}

	for (i = 0; i < connector.count_props; i++) {
		struct drm_mode_get_property property = {
			.prop_id = property_ids[i],
		};

		if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &property) < 0) {
			perror("GETPROPERTY for writeback");
			return -1;
		}
		if (!strcmp((const char *)property.name, name)) {
			*property_id = property.prop_id;
			return 0;
		}
	}

	fprintf(stderr, "writeback connector lacks %s\n", name);
	return -1;
}

static int expect_foreign_writeback_denied(
	int fd, const struct test_display *display)
{
	struct test_framebuffer destination = {};
	uint32_t objects[1];
	uint32_t property_counts[1] = { 2 };
	uint32_t properties[2];
	uint64_t values[2];
	struct drm_mode_atomic atomic = {
		.flags = DRM_MODE_ATOMIC_ALLOW_MODESET,
		.count_objs = 1,
		.objs_ptr = (uint64_t)(uintptr_t)objects,
		.count_props_ptr = (uint64_t)(uintptr_t)property_counts,
		.props_ptr = (uint64_t)(uintptr_t)properties,
		.prop_values_ptr = (uint64_t)(uintptr_t)values,
	};
	uint32_t writeback_connector;
	int ret = -1;

	if (find_writeback_connector(fd, &writeback_connector) ||
	    find_connector_property(fd, writeback_connector, "CRTC_ID",
				    &properties[0]) ||
	    find_connector_property(fd, writeback_connector, "WRITEBACK_FB_ID",
				    &properties[1]) ||
	    create_test_framebuffer(fd, display->mode.hdisplay,
				    display->mode.vdisplay, &destination))
		goto out;

	objects[0] = writeback_connector;
	values[0] = display->crtc_id;
	values[1] = destination.fb_id;
	if (ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic) == 0) {
		fprintf(stderr, "writeback of foreign content unexpectedly succeeded\n");
		goto out;
	}
	if (errno != EACCES) {
		fprintf(stderr, "foreign writeback returned %s, expected %s\n",
			strerror(errno), strerror(EACCES));
		goto out;
	}
	ret = 0;

out:
	destroy_test_framebuffer(fd, &destination);
	return ret;
}

static int expect_plain_capture_denied(int fd, uint32_t connector_id)
{
	struct drm_castkms_capture_attach_monitor attach = {
		.connector_id = connector_id,
	};
	struct drm_castkms_capture_detach_monitor detach = {
		.connector_id = connector_id,
	};
	struct drm_castkms_capture_set_output_edid edid = {
		.connector_id = connector_id,
	};
	struct drm_castkms_capture_register_buffer register_buffer = {
		.stream_id = 1,
		.fb_id = 1,
		.flags = DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC,
	};
	struct drm_castkms_capture_queue_buffer queue_buffer = {
		.stream_id = 1,
		.buffer_id = 1,
		.flags = DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC,
	};
	struct drm_castkms_capture_read_cursor_bitmap cursor = {
		.stream_id = 1,
		.buffer_id = 1,
	};
	struct drm_castkms_get_grant get_grant = {};

	if (expect_ioctl_errno(fd, DRM_IOCTL_CASTKMS_GET_GRANT, &get_grant,
			       ENODATA, "plain-fd GET_GRANT") ||
	    expect_ioctl_errno(fd, DRM_IOCTL_CASTKMS_CAPTURE_ATTACH_MONITOR,
			       &attach, EACCES, "plain-fd ATTACH_MONITOR") ||
	    expect_ioctl_errno(fd, DRM_IOCTL_CASTKMS_CAPTURE_DETACH_MONITOR,
			       &detach, EACCES, "plain-fd DETACH_MONITOR") ||
	    expect_ioctl_errno(fd, DRM_IOCTL_CASTKMS_CAPTURE_SET_OUTPUT_EDID,
			       &edid, EACCES, "plain-fd SET_OUTPUT_EDID") ||
	    expect_ioctl_errno(fd, DRM_IOCTL_CASTKMS_CAPTURE_REGISTER_BUFFER,
			       &register_buffer, EACCES,
			       "plain-fd REGISTER_BUFFER") ||
	    expect_ioctl_errno(fd, DRM_IOCTL_CASTKMS_CAPTURE_QUEUE_BUFFER,
			       &queue_buffer, EACCES, "plain-fd QUEUE_BUFFER") ||
	    expect_ioctl_errno(fd, DRM_IOCTL_CASTKMS_CAPTURE_READ_CURSOR_BITMAP,
			       &cursor, EACCES, "plain-fd READ_CURSOR_BITMAP"))
		return -1;

	return 0;
}

static int expect_holder_cannot_create_grant(int fd, uint32_t connector_id)
{
	struct drm_castkms_create_grant create = {
		.connector_id = connector_id,
		.rights = DRM_CASTKMS_GRANT_CAPTURE_PIXELS,
	};

	return expect_ioctl_errno(fd, DRM_IOCTL_CASTKMS_CREATE_GRANT, &create,
				  EACCES, "grant-holder CREATE_GRANT");
}

static int expect_create_flag_namespaces(int fd, uint32_t connector_id)
{
	struct drm_castkms_create_grant create = {
		.connector_id = connector_id,
		.rights = DRM_CASTKMS_GRANT_CAPTURE_PIXELS,
		.flags = O_CLOEXEC,
	};

	if (expect_ioctl_errno(fd, DRM_IOCTL_CASTKMS_CREATE_GRANT, &create,
			       EINVAL, "CREATE_GRANT open flag in grant flags"))
		return -1;
	create = (struct drm_castkms_create_grant) {
		.connector_id = connector_id,
		.rights = DRM_CASTKMS_GRANT_CAPTURE_PIXELS,
		.flags = DRM_CASTKMS_GRANT_CREATE_ADMIN |
			 DRM_CASTKMS_GRANT_CREATE_DELEGATED,
	};
	if (expect_ioctl_errno(fd, DRM_IOCTL_CASTKMS_CREATE_GRANT, &create,
			       EINVAL, "CREATE_GRANT conflicting grant flags"))
		return -1;

	create = (struct drm_castkms_create_grant) {
		.connector_id = connector_id,
		.rights = DRM_CASTKMS_GRANT_CAPTURE_PIXELS,
		.fd_flags = O_WRONLY,
	};
	return expect_ioctl_errno(fd, DRM_IOCTL_CASTKMS_CREATE_GRANT, &create,
				  EINVAL, "CREATE_GRANT access mode in fd flags");
}

static int test_attachment_lifecycle(int grant_fd, uint32_t connector_id)
{
	struct drm_castkms_cec_bind_transport bind = {
		.connector_id = connector_id,
	};
	struct drm_castkms_cec_query_caps caps = {
		.connector_id = connector_id,
	};
	struct drm_castkms_capture_attach_monitor attach = {
		.connector_id = connector_id,
	};
	struct drm_castkms_capture_detach_monitor detach = {
		.connector_id = connector_id,
	};
	bool cec_bound = false;
	int cec_query_ret;

	if (ioctl(grant_fd, DRM_IOCTL_CASTKMS_CAPTURE_ATTACH_MONITOR,
		  &attach) < 0) {
		perror("grant holder ATTACH_MONITOR");
		return -1;
	}

	cec_query_ret = ioctl(grant_fd, DRM_IOCTL_CASTKMS_CEC_QUERY_CAPS, &caps);
	if (cec_query_ret < 0 && errno != ENOTTY) {
		perror("grant holder CEC_QUERY_CAPS");
		return -1;
	}
	if (!cec_query_ret && caps.has_adapter) {
		struct drm_castkms_cec_set_transport_state online;

		if (ioctl(grant_fd, DRM_IOCTL_CASTKMS_CEC_BIND_TRANSPORT,
			  &bind) < 0) {
			perror("grant holder CEC_BIND_TRANSPORT");
			return -1;
		}
		online = (struct drm_castkms_cec_set_transport_state) {
			.connector_id = connector_id,
			.transport_id = bind.transport_id,
			.flags = DRM_CASTKMS_CEC_TRANSPORT_ONLINE,
		};
		if (ioctl(grant_fd, DRM_IOCTL_CASTKMS_CEC_SET_TRANSPORT_STATE,
			  &online) < 0) {
			perror("grant holder CEC_SET_TRANSPORT_STATE");
			return -1;
		}
		cec_bound = true;
	}

	if (ioctl(grant_fd, DRM_IOCTL_CASTKMS_CAPTURE_DETACH_MONITOR,
		  &detach) < 0) {
		perror("grant holder DETACH_MONITOR");
		return -1;
	}

	if (cec_bound) {
		struct drm_castkms_cec_get_state state = {
			.connector_id = connector_id,
			.transport_id = bind.transport_id,
		};
		struct drm_castkms_cec_unbind_transport unbind = {
			.connector_id = connector_id,
			.transport_id = bind.transport_id,
		};

		if (ioctl(grant_fd, DRM_IOCTL_CASTKMS_CEC_GET_STATE, &state) < 0) {
			perror("grant holder CEC_GET_STATE after detach");
			return -1;
		}
		if (!(state.state_flags &
		      DRM_CASTKMS_CEC_STATE_TRANSPORT_ONLINE) ||
		    (state.state_flags &
		     DRM_CASTKMS_CEC_STATE_MONITOR_ATTACHED)) {
			fprintf(stderr,
				"CEC detach returned unexpected state flags %#x\n",
				state.state_flags);
			return -1;
		}
		if (ioctl(grant_fd, DRM_IOCTL_CASTKMS_CEC_UNBIND_TRANSPORT,
			  &unbind) < 0) {
			perror("grant holder CEC_UNBIND_TRANSPORT");
			return -1;
		}
		printf("grant_cec_detach=pass\n");
	}

	return 0;
}

int main(int argc, char **argv)
{
	const uint32_t full_rights = DRM_CASTKMS_GRANT_CAPTURE_PIXELS |
		DRM_CASTKMS_GRANT_MANAGE_ATTACHMENT |
		DRM_CASTKMS_GRANT_UPDATE_EDID |
		DRM_CASTKMS_GRANT_READ_CURSOR |
		DRM_CASTKMS_GRANT_MANAGE_CEC;
	struct drm_castkms_capture_attach_monitor attach;
	struct drm_castkms_capture_start admin_stream = {};
	struct drm_castkms_capture_start delegated_stream = {};
	struct drm_castkms_get_grant issuer_query;
	struct test_framebuffer admin_destination = {};
	struct test_framebuffer delegated_destination = {};
	struct test_framebuffer master_b_source = {};
	struct test_display display = {};
	uint32_t masterless_admin_id = 0;
	uint32_t issuer_close_id = 0;
	uint32_t detached_delegated_id = 0;
	uint32_t delegated_buffer_id = 0;
	uint32_t delegated_id = 0;
	uint32_t master_b_grant_id = 0;
	uint32_t admin_buffer_id = 0;
	uint32_t normal_id = 0;
	uint32_t admin_id = 0;
	uint32_t connector_id;
	int masterless_admin_fd = -1;
	int issuer_close_holder = -1;
	int detached_delegated_fd = -1;
	int delegated_creator = -1;
	int delegated_fd = -1;
	int master_b_grant_fd = -1;
	int admin_fd = -1;
	int normal_fd = -1;
	int issuer2 = -1;
	int issuer = -1;
	int master_b = -1;
	int created_fd = -1;
	bool admin_stream_prepared = false;
	bool delegated_stream_prepared = false;
	int ret = EXIT_FAILURE;

	if (argc != 3 || parse_connector_id(argv[2], &connector_id)) {
		fprintf(stderr, "usage: %s DRM-DEVICE CONNECTOR-ID\n", argv[0]);
		return EXIT_FAILURE;
	}

	issuer = open(argv[1], O_RDWR | O_CLOEXEC);
	if (issuer < 0) {
		perror("open issuer");
		goto out;
	}
	if (ioctl(issuer, DRM_IOCTL_SET_MASTER, 0) < 0) {
		perror("issuer DRM_IOCTL_SET_MASTER");
		goto out;
	}
	if (expect_plain_capture_denied(issuer, connector_id))
		goto out;
	printf("grant_plain_fd_denied=pass\n");
	if (expect_create_flag_namespaces(issuer, connector_id))
		goto out;
	printf("grant_flag_namespaces=pass\n");

	if (create_grant(issuer, connector_id, full_rights, 0,
			 &created_fd, &normal_id))
		goto out;
	normal_fd = pass_fd(created_fd);
	if (normal_fd < 0)
		goto out;
	close(created_fd);
	created_fd = -1;
	if (!(fcntl(normal_fd, F_GETFD) & FD_CLOEXEC) ||
	    expect_holder_state(normal_fd, normal_id,
				DRM_CASTKMS_GRANT_STATE_PENDING, 0) ||
	    expect_holder_cannot_create_grant(normal_fd, connector_id))
		goto out;
	printf("grant_scm_rights=pass\n");

	if (test_attachment_lifecycle(normal_fd, connector_id))
		goto out;
	printf("grant_holder_attachment=pass\n");
	if (setup_display(issuer, normal_fd, connector_id, &display) ||
	    expect_holder_state(normal_fd, normal_id,
				DRM_CASTKMS_GRANT_STATE_ACTIVE, 0) ||
	    test_capture_frame(normal_fd, &display))
		goto out;
	printf("grant_capture_frame=pass\n");
	if (test_master_cleanup_generation(issuer, normal_fd, &display))
		goto out;
	printf("grant_master_cleanup_generation=pass\n");

	delegated_creator = open(argv[1], O_RDWR | O_CLOEXEC);
	if (delegated_creator < 0) {
		perror("open delegated grant creator");
		goto out;
	}
	if (expect_create_grant_errno(
		    delegated_creator, connector_id, 0, EACCES,
		    "privileged non-master CREATE_GRANT without DELEGATED") ||
	    expect_create_grant_errno(
		    issuer, connector_id, DRM_CASTKMS_GRANT_CREATE_DELEGATED,
		    EAGAIN, "current master CREATE_GRANT with DELEGATED") ||
	    create_grant(delegated_creator, connector_id,
			 DRM_CASTKMS_GRANT_CAPTURE_PIXELS,
			 DRM_CASTKMS_GRANT_CREATE_DELEGATED,
			 &delegated_fd, &delegated_id) ||
	    expect_holder_state(delegated_fd, delegated_id,
				DRM_CASTKMS_GRANT_STATE_ACTIVE,
				DRM_CASTKMS_GRANT_FLAG_DELEGATED) ||
	    prepare_capture_stream(delegated_fd, &display, &delegated_stream,
				   &delegated_destination,
				   &delegated_buffer_id))
		goto out;
	delegated_stream_prepared = true;
	close(delegated_creator);
	delegated_creator = -1;
	if (expect_holder_state(delegated_fd, delegated_id,
				DRM_CASTKMS_GRANT_STATE_ACTIVE,
				DRM_CASTKMS_GRANT_FLAG_DELEGATED) ||
	    capture_prepared_stream(
		    delegated_fd, &delegated_stream, delegated_buffer_id,
		    UINT64_C(0x44454c4547415445)))
		goto out;
	printf("grant_delegated_creator_close=pass\n");
	if (ioctl(issuer, DRM_IOCTL_DROP_MASTER, 0) < 0) {
		perror("issuer DRM_IOCTL_DROP_MASTER for delegated stream");
		goto out;
	}
	if (expect_holder_state(normal_fd, normal_id,
				DRM_CASTKMS_GRANT_STATE_SUSPENDED_NO_MASTER,
				0) ||
	    expect_holder_state(delegated_fd, delegated_id,
				DRM_CASTKMS_GRANT_STATE_SUSPENDED_NO_MASTER,
				DRM_CASTKMS_GRANT_FLAG_DELEGATED) ||
	    expect_create_grant_errno(
		    issuer, connector_id, DRM_CASTKMS_GRANT_CREATE_DELEGATED,
		    EAGAIN, "masterless CREATE_GRANT with DELEGATED"))
		goto out;
	if (ioctl(issuer, DRM_IOCTL_SET_MASTER, 0) < 0) {
		perror("issuer DRM_IOCTL_SET_MASTER after delegated stream");
		goto out;
	}
	if (expect_holder_state(normal_fd, normal_id,
				DRM_CASTKMS_GRANT_STATE_ACTIVE, 0) ||
	    expect_holder_state(delegated_fd, delegated_id,
				DRM_CASTKMS_GRANT_STATE_ACTIVE,
				DRM_CASTKMS_GRANT_FLAG_DELEGATED) ||
	    expect_capture_stream_collected(
		    delegated_fd, &delegated_stream, delegated_buffer_id))
		goto out;
	delegated_stream_prepared = false;
	destroy_test_framebuffer(delegated_fd, &delegated_destination);
	printf("grant_delegated_stream_cleanup=pass\n");

	if (create_grant(issuer, connector_id,
			 DRM_CASTKMS_GRANT_CAPTURE_PIXELS,
			 DRM_CASTKMS_GRANT_CREATE_ADMIN,
			 &admin_fd, &admin_id) ||
	    expect_holder_state(admin_fd, admin_id,
				DRM_CASTKMS_GRANT_STATE_ACTIVE,
				DRM_CASTKMS_GRANT_FLAG_ADMIN) ||
	    prepare_capture_stream(admin_fd, &display, &admin_stream,
				   &admin_destination, &admin_buffer_id))
		goto out;
	admin_stream_prepared = true;
	printf("grant_admin_stream_before_handoff=pass\n");

	if (ioctl(issuer, DRM_IOCTL_DROP_MASTER, 0) < 0) {
		perror("issuer DRM_IOCTL_DROP_MASTER");
		goto out;
	}
	if (expect_holder_state(normal_fd, normal_id,
				DRM_CASTKMS_GRANT_STATE_SUSPENDED_NO_MASTER,
				0) ||
	    expect_holder_state(delegated_fd, delegated_id,
				DRM_CASTKMS_GRANT_STATE_SUSPENDED_NO_MASTER,
				DRM_CASTKMS_GRANT_FLAG_DELEGATED) ||
	    expect_holder_state(admin_fd, admin_id,
				DRM_CASTKMS_GRANT_STATE_SUSPENDED_NO_MASTER,
				DRM_CASTKMS_GRANT_FLAG_ADMIN))
		goto out;
	printf("grant_master_drop_suspends=pass\n");

	if (create_grant(issuer, connector_id,
			 DRM_CASTKMS_GRANT_CAPTURE_PIXELS,
			 DRM_CASTKMS_GRANT_CREATE_ADMIN,
			 &masterless_admin_fd, &masterless_admin_id) ||
	    expect_holder_state(masterless_admin_fd, masterless_admin_id,
				DRM_CASTKMS_GRANT_STATE_SUSPENDED_NO_MASTER,
				DRM_CASTKMS_GRANT_FLAG_ADMIN))
		goto out;
	attach = (struct drm_castkms_capture_attach_monitor) {
		.connector_id = connector_id,
	};
	if (expect_ioctl_errno(masterless_admin_fd,
			       DRM_IOCTL_CASTKMS_CAPTURE_ATTACH_MONITOR,
			       &attach, EACCES,
			       "capture-only grant ATTACH_MONITOR"))
		goto out;
	close(masterless_admin_fd);
	masterless_admin_fd = -1;
	printf("grant_admin_masterless_create=pass\n");

	master_b = open(argv[1], O_RDWR | O_CLOEXEC);
	if (master_b < 0) {
		perror("open replacement master");
		goto out;
	}
	if (expect_plain_capture_denied(master_b, connector_id) ||
	    expect_holder_state(normal_fd, normal_id,
					DRM_CASTKMS_GRANT_STATE_SUSPENDED_OTHER_MASTER,
					0) ||
	    expect_holder_state(delegated_fd, delegated_id,
					DRM_CASTKMS_GRANT_STATE_SUSPENDED_OTHER_MASTER,
					DRM_CASTKMS_GRANT_FLAG_DELEGATED) ||
	    expect_holder_state(admin_fd, admin_id,
					DRM_CASTKMS_GRANT_STATE_SUSPENDED_FOREIGN_CONTENT,
					DRM_CASTKMS_GRANT_FLAG_ADMIN))
		goto out;
	printf("grant_admin_master_handoff=pass\n");

	if (create_grant(master_b, connector_id,
			 DRM_CASTKMS_GRANT_CAPTURE_PIXELS, 0,
			 &master_b_grant_fd, &master_b_grant_id) ||
	    expect_holder_state(
		    master_b_grant_fd, master_b_grant_id,
		    DRM_CASTKMS_GRANT_STATE_SUSPENDED_FOREIGN_CONTENT, 0) ||
	    expect_capture_start_errno(master_b_grant_fd, display.crtc_id,
				       ESTALE) ||
	    test_crc_access(master_b, EACCES) ||
	    expect_foreign_writeback_denied(master_b, &display))
		goto out;
	printf("grant_foreign_content_blocked=pass\n");

	if (create_test_framebuffer(master_b, display.mode.hdisplay,
				    display.mode.vdisplay, &master_b_source) ||
	    set_display_framebuffer(master_b, connector_id, &display,
				    &master_b_source) ||
	    expect_holder_state(master_b_grant_fd, master_b_grant_id,
				DRM_CASTKMS_GRANT_STATE_ACTIVE, 0) ||
	    expect_holder_state(delegated_fd, delegated_id,
				DRM_CASTKMS_GRANT_STATE_SUSPENDED_OTHER_MASTER,
				DRM_CASTKMS_GRANT_FLAG_DELEGATED) ||
	    expect_holder_state(admin_fd, admin_id,
				DRM_CASTKMS_GRANT_STATE_ACTIVE,
				DRM_CASTKMS_GRANT_FLAG_ADMIN) ||
	    expect_capture_stream_collected(admin_fd, &admin_stream,
					    admin_buffer_id))
		goto out;
	admin_stream_prepared = false;
	destroy_test_framebuffer(admin_fd, &admin_destination);
	if (test_capture_frame(admin_fd, &display) ||
	    test_capture_frame(master_b_grant_fd, &display) ||
	    test_crc_access(master_b, 0))
		goto out;
	printf("grant_admin_stream_handoff=pass\n");
	printf("grant_new_master_content=pass\n");

	close(master_b_grant_fd);
	master_b_grant_fd = -1;
	close(master_b);
	master_b = -1;
	master_b_source = (struct test_framebuffer) {};

	if (ioctl(issuer, DRM_IOCTL_SET_MASTER, 0) < 0) {
		perror("issuer master reacquire");
		goto out;
	}
	if (expect_holder_state(normal_fd, normal_id,
					DRM_CASTKMS_GRANT_STATE_SUSPENDED_FOREIGN_CONTENT,
					0) ||
	    expect_holder_state(delegated_fd, delegated_id,
				DRM_CASTKMS_GRANT_STATE_SUSPENDED_FOREIGN_CONTENT,
				DRM_CASTKMS_GRANT_FLAG_DELEGATED) ||
	    expect_holder_state(admin_fd, admin_id,
				DRM_CASTKMS_GRANT_STATE_SUSPENDED_FOREIGN_CONTENT,
				DRM_CASTKMS_GRANT_FLAG_ADMIN) ||
	    expect_capture_start_errno(normal_fd, display.crtc_id, ESTALE) ||
	    set_display_framebuffer(issuer, connector_id, &display,
				    &display.source) ||
	    expect_holder_state(normal_fd, normal_id,
				DRM_CASTKMS_GRANT_STATE_ACTIVE, 0) ||
	    expect_holder_state(delegated_fd, delegated_id,
				DRM_CASTKMS_GRANT_STATE_ACTIVE,
				DRM_CASTKMS_GRANT_FLAG_DELEGATED) ||
	    test_capture_frame(normal_fd, &display) ||
	    test_capture_frame(delegated_fd, &display))
		goto out;
	printf("grant_master_revivify=pass\n");
	printf("grant_delegated_master_revivify=pass\n");

	if (revoke_grant(issuer, delegated_id) ||
	    expect_revoke_event(delegated_fd, delegated_id, -EKEYREVOKED) ||
	    expect_holder_state(delegated_fd, delegated_id,
				DRM_CASTKMS_GRANT_STATE_REVOKED,
				DRM_CASTKMS_GRANT_FLAG_DELEGATED))
		goto out;
	close(delegated_fd);
	delegated_fd = -1;
	if (expect_revoke_grant_errno(
		    issuer, delegated_id, ENOENT,
		    "root REVOKE_GRANT after delegated holder close"))
		goto out;
	printf("grant_delegated_root_revoke=pass\n");

	delegated_creator = open(argv[1], O_RDWR | O_CLOEXEC);
	if (delegated_creator < 0) {
		perror("open final-holder delegated grant creator");
		goto out;
	}
	if (create_grant(delegated_creator, connector_id,
			 DRM_CASTKMS_GRANT_CAPTURE_PIXELS,
			 DRM_CASTKMS_GRANT_CREATE_DELEGATED,
			 &detached_delegated_fd, &detached_delegated_id))
		goto out;
	close(delegated_creator);
	delegated_creator = -1;
	if (expect_holder_state(detached_delegated_fd,
				detached_delegated_id,
				DRM_CASTKMS_GRANT_STATE_ACTIVE,
				DRM_CASTKMS_GRANT_FLAG_DELEGATED))
		goto out;
	close(detached_delegated_fd);
	detached_delegated_fd = -1;
	if (expect_revoke_grant_errno(
		    issuer, detached_delegated_id, ENOENT,
		    "root REVOKE_GRANT after final delegated holder close"))
		goto out;
	printf("grant_delegated_final_holder_close=pass\n");

	if (revoke_grant(issuer, normal_id) ||
	    expect_revoke_event(normal_fd, normal_id, -EKEYREVOKED) ||
	    expect_holder_state(normal_fd, normal_id,
				DRM_CASTKMS_GRANT_STATE_REVOKED, 0))
		goto out;
	attach = (struct drm_castkms_capture_attach_monitor) {
		.connector_id = connector_id,
	};
	if (expect_ioctl_errno(normal_fd,
			       DRM_IOCTL_CASTKMS_CAPTURE_ATTACH_MONITOR,
			       &attach, EKEYREVOKED,
			       "revoked-grant ATTACH_MONITOR"))
		goto out;
	printf("grant_explicit_revoke=pass\n");
	close(normal_fd);
	normal_fd = -1;

	close(admin_fd);
	admin_fd = -1;
	issuer_query = (struct drm_castkms_get_grant) {
		.grant_id = admin_id,
	};
	if (expect_ioctl_errno(issuer, DRM_IOCTL_CASTKMS_GET_GRANT,
			       &issuer_query, ENODATA,
			       "final-holder GET_GRANT"))
		goto out;
	printf("grant_final_holder_close=pass\n");

	issuer2 = open(argv[1], O_RDWR | O_CLOEXEC);
	if (issuer2 < 0) {
		perror("open secondary issuer");
		goto out;
	}
	if (create_grant(issuer2, connector_id,
			 DRM_CASTKMS_GRANT_CAPTURE_PIXELS,
			 DRM_CASTKMS_GRANT_CREATE_ADMIN,
			 &issuer_close_holder, &issuer_close_id))
		goto out;
	if (expect_holder_state(issuer_close_holder, issuer_close_id,
				DRM_CASTKMS_GRANT_STATE_PENDING,
				DRM_CASTKMS_GRANT_FLAG_ADMIN))
		goto out;
	close(issuer2);
	issuer2 = -1;
	if (expect_revoke_event(issuer_close_holder, issuer_close_id,
				-EKEYREVOKED) ||
	    expect_holder_state(issuer_close_holder, issuer_close_id,
				DRM_CASTKMS_GRANT_STATE_REVOKED,
				DRM_CASTKMS_GRANT_FLAG_ADMIN))
		goto out;
	printf("grant_issuer_close_revoke=pass\n");

	printf("grant_lifecycle=pass\n");
	ret = EXIT_SUCCESS;

out:
	if (created_fd >= 0)
		close(created_fd);
	if (masterless_admin_fd >= 0)
		close(masterless_admin_fd);
	if (issuer_close_holder >= 0)
		close(issuer_close_holder);
	if (detached_delegated_fd >= 0)
		close(detached_delegated_fd);
	if (delegated_creator >= 0)
		close(delegated_creator);
	if (delegated_stream_prepared && delegated_fd >= 0)
		stop_capture(delegated_fd, delegated_stream.stream_id);
	if (delegated_fd >= 0)
		destroy_test_framebuffer(delegated_fd,
					   &delegated_destination);
	if (delegated_fd >= 0)
		close(delegated_fd);
	if (master_b_grant_fd >= 0)
		close(master_b_grant_fd);
	if (master_b >= 0)
		destroy_test_framebuffer(master_b, &master_b_source);
	if (master_b >= 0)
		close(master_b);
	if (normal_fd >= 0)
		close(normal_fd);
	if (admin_stream_prepared)
		stop_capture(admin_fd, admin_stream.stream_id);
	destroy_test_framebuffer(admin_fd, &admin_destination);
	if (admin_fd >= 0)
		close(admin_fd);
	if (issuer2 >= 0)
		close(issuer2);
	if (issuer >= 0)
		destroy_test_framebuffer(issuer, &display.source);
	if (issuer >= 0)
		close(issuer);
	return ret;
}
