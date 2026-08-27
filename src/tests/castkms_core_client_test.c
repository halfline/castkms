// SPDX-License-Identifier: GPL-2.0-only

/* Kept first so every KUnit build checks this header's self-containment. */
#include "../castkms_grant_file.h"

#include <linux/err.h>
#include <linux/dma-resv.h>
#include <linux/kref.h>
#include <linux/slab.h>

#include <kunit/test.h>

#include <drm/drm_auth.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_vblank.h>

#include "../castkms_capture.h"
#include "../castkms_capture_authority.h"
#include "../castkms_capture_owner.h"
#include "../castkms_config.h"
#include "../castkms_connector.h"
#include "../castkms_drv.h"
#include "../castkms_frame_dispatch.h"
#include "../castkms_output.h"

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

#define CORE_CLIENT_WIDTH 64U
#define CORE_CLIENT_HEIGHT 64U

struct castkms_core_client {
	struct castkms_config *config;
	struct castkms_output *output;
	struct castkms_connector *connector;
	struct drm_master *master;
	struct drm_file owner_file;
	struct drm_framebuffer *framebuffer;
	struct castkms_capture_authority *authority;
	struct castkms_capture_authority_resource stream_resource;
	struct castkms_capture_stream *stream;
	struct castkms_capture_buffer *buffer;
	struct castkms_capture_request request;
	struct castkms_capture_result result;
	struct dma_fence *completion_fence;
	u64 mode_generation;
	int stop_status;
	int revoke_status;
	unsigned int stop_calls;
	unsigned int revoke_calls;
	unsigned int release_calls;
	unsigned int request_calls;
	bool callback_saw_completion_fence_signaled;
	bool stream_resource_registered;
	bool vblank_on;
};

static const struct drm_framebuffer_funcs castkms_core_client_fb_funcs = {
	.destroy = drm_gem_fb_destroy,
	.create_handle = drm_gem_fb_create_handle,
};

static struct drm_framebuffer *
castkms_core_client_create_framebuffer(struct drm_device *dev)
{
	const size_t size = CORE_CLIENT_WIDTH * CORE_CLIENT_HEIGHT * 4;
	struct drm_gem_shmem_object *shmem;
	struct drm_mode_fb_cmd2 mode = {
		.width = CORE_CLIENT_WIDTH,
		.height = CORE_CLIENT_HEIGHT,
		.pixel_format = DRM_FORMAT_XRGB8888,
		.pitches[0] = CORE_CLIENT_WIDTH * 4,
	};
	struct drm_framebuffer *framebuffer;
	int ret;

	shmem = drm_gem_shmem_create(dev, size);
	if (IS_ERR(shmem))
		return ERR_CAST(shmem);

	framebuffer = kzalloc_obj(*framebuffer);
	if (!framebuffer) {
		drm_gem_object_put(&shmem->base);
		return ERR_PTR(-ENOMEM);
	}

	drm_helper_mode_fill_fb_struct(dev, framebuffer,
				       drm_format_info(mode.pixel_format), &mode);
	framebuffer->obj[0] = &shmem->base;
	ret = drm_framebuffer_init(dev, framebuffer,
				   &castkms_core_client_fb_funcs);
	if (ret) {
		drm_gem_object_put(&shmem->base);
		kfree(framebuffer);
		return ERR_PTR(ret);
	}

	return framebuffer;
}

static void castkms_core_client_complete(struct castkms_capture_request *request,
					 const struct castkms_capture_result *result)
{
	struct castkms_core_client *client =
		container_of(request, struct castkms_core_client, request);

	client->result = *result;
	if (client->completion_fence)
		client->callback_saw_completion_fence_signaled =
			dma_fence_is_signaled(client->completion_fence);
	client->request_calls++;
}

static bool castkms_core_client_stream_needs_cleanup(
	struct castkms_capture_authority_resource *resource,
	enum castkms_capture_authority_cleanup_reason reason, u64 generation)
{
	struct castkms_core_client *client =
		container_of(resource, struct castkms_core_client,
			     stream_resource);

	switch (reason) {
	case CASTKMS_CAPTURE_AUTHORITY_CLEANUP_MASTER_EPOCH:
		return castkms_capture_authority_generation_is_stale(
			castkms_capture_stream_authority_generation(client->stream),
			generation);
	case CASTKMS_CAPTURE_AUTHORITY_CLEANUP_DISCONNECT:
		return true;
	default:
		return false;
	}
}

static void castkms_core_client_stream_revoke(
	struct castkms_capture_authority_resource *resource, int status)
{
	struct castkms_core_client *client =
		container_of(resource, struct castkms_core_client,
			     stream_resource);
	struct castkms_capture_stream *stream = client->stream;

	client->stop_calls++;
	client->stop_status = status;
	client->stream_resource_registered = false;
	client->stream = NULL;
	client->buffer = NULL;
	if (stream)
		castkms_capture_stream_destroy(stream, status);
}

static const struct castkms_capture_authority_resource_ops
castkms_core_client_stream_resource_ops = {
	.needs_cleanup = castkms_core_client_stream_needs_cleanup,
	.revoke = castkms_core_client_stream_revoke,
};

static void
castkms_core_client_revoked(struct castkms_capture_authority *authority,
			    int status, void *data)
{
	struct castkms_core_client *client = data;

	(void)authority;
	client->revoke_calls++;
	client->revoke_status = status;
}

static void
castkms_core_client_release(struct castkms_capture_authority *authority,
			    void *data)
{
	struct castkms_core_client *client = data;

	(void)authority;
	client->release_calls++;
}

static const struct castkms_capture_authority_ops
castkms_core_client_authority_ops = {
	.revoked = castkms_core_client_revoked,
	.release = castkms_core_client_release,
};

static void castkms_core_client_stop_stream(struct castkms_core_client *client,
					    int status)
{
	struct castkms_capture_stream *stream = client->stream;

	if (!stream)
		return;
	if (client->stream_resource_registered) {
		WARN_ON(!castkms_capture_authority_unregister_resource(
			client->authority, &client->stream_resource));
		client->stream_resource_registered = false;
	}
	client->stream = NULL;
	client->buffer = NULL;
	castkms_capture_stream_destroy(stream, status);
}

static void castkms_core_client_cleanup(void *data)
{
	struct castkms_core_client *client = data;
	unsigned long flags;

	if (client->authority) {
		castkms_capture_authority_revoke(client->authority, -ECANCELED);
		castkms_core_client_stop_stream(client, -ECANCELED);
		castkms_capture_authority_put(client->authority);
		client->authority = NULL;
	} else if (client->stream) {
		castkms_capture_stream_destroy(client->stream, -ECANCELED);
		client->stream = NULL;
		client->buffer = NULL;
	}
	if (client->framebuffer) {
		drm_framebuffer_put(client->framebuffer);
		client->framebuffer = NULL;
	}
	dma_fence_put(client->completion_fence);
	client->completion_fence = NULL;
	if (client->vblank_on) {
		drm_crtc_vblank_off(&client->output->crtc);
		client->vblank_on = false;
	}
	if (client->output) {
		spin_lock_irqsave(&client->output->capture.state_lock, flags);
		client->output->capture.active = false;
		client->output->capture.width = 0;
		client->output->capture.height = 0;
		spin_unlock_irqrestore(&client->output->capture.state_lock, flags);
		client->output->crtc.state->active = false;
		client->output->crtc.state->enable = false;
	}
	if (client->config && client->config->dev)
		castkms_destroy(client->config);
	if (client->config) {
		castkms_config_destroy(client->config);
		client->config = NULL;
	}
}

static void
castkms_core_client_activate_output(struct castkms_core_client *client)
{
	struct castkms_capture_owner_state *owners =
		&client->config->dev->capture_owners;
	struct drm_crtc_state *state = client->output->crtc.state;
	struct drm_display_mode mode = {
		.clock = 25175,
		.hdisplay = 640,
		.hsync_start = 656,
		.hsync_end = 752,
		.htotal = 800,
		.vdisplay = 480,
		.vsync_start = 490,
		.vsync_end = 492,
		.vtotal = 525,
	};
	unsigned long flags;

	client->master->dev = &client->config->dev->drm;
	kref_init(&client->master->refcount);

	spin_lock_irqsave(&owners->lock, flags);
	owners->master = drm_master_get(client->master);
	owners->master_file = &client->owner_file;
	owners->master_active = true;
	spin_unlock_irqrestore(&owners->lock, flags);

	spin_lock_irqsave(&client->output->lock, flags);
	client->output->capture_owner = drm_master_get(client->master);
	client->output->capture_owner_updating = false;
	spin_unlock_irqrestore(&client->output->lock, flags);

	spin_lock_irqsave(&client->output->capture.state_lock, flags);
	client->output->capture.mode_generation++;
	client->output->capture.width = CORE_CLIENT_WIDTH;
	client->output->capture.height = CORE_CLIENT_HEIGHT;
	client->output->capture.active = true;
	spin_unlock_irqrestore(&client->output->capture.state_lock, flags);

	drm_mode_set_crtcinfo(&mode, 0);
	state->mode = mode;
	state->adjusted_mode = mode;
	client->output->crtc.mode = mode;
	client->output->crtc.hwmode = mode;
	state->enable = true;
	state->active = true;
	drm_crtc_vblank_on(&client->output->crtc);
	client->vblank_on = true;
}

static int castkms_core_client_start_stream(struct castkms_core_client *client)
{
	int ret;

	ret = castkms_capture_authority_begin(client->authority,
					      &client->connector->base,
					      CASTKMS_CAPTURE_AUTHORITY_CAPTURE_PIXELS);
	if (!ret) {
		client->stream = castkms_capture_stream_create(client->output,
							       client->authority,
							       true,
							       &client->mode_generation);
		if (IS_ERR(client->stream)) {
			ret = PTR_ERR(client->stream);
			client->stream = NULL;
		} else {
			ret = castkms_capture_stream_attach(client->stream);
			if (!ret) {
				ret = castkms_capture_authority_register_resource(
					client->authority,
					&client->stream_resource,
					&castkms_core_client_stream_resource_ops);
				if (!ret)
					client->stream_resource_registered = true;
			}
		}
		castkms_capture_authority_end(client->authority);
	}
	if (ret)
		castkms_core_client_stop_stream(client, -ECANCELED);

	return ret;
}

static int castkms_core_client_create_buffer(struct castkms_core_client *client)
{
	client->buffer = castkms_capture_buffer_create(client->stream,
						       client->framebuffer, NULL, NULL,
						       CASTKMS_CAPTURE_SYNC_IMPLICIT,
						       client->mode_generation);
	if (IS_ERR(client->buffer)) {
		int ret = PTR_ERR(client->buffer);

		client->buffer = NULL;
		return ret;
	}

	return 0;
}

static int castkms_core_client_submit(struct castkms_core_client *client)
{
	int ret;

	client->request.complete = castkms_core_client_complete;
	ret = castkms_capture_authority_begin(client->authority,
					      &client->connector->base,
					      CASTKMS_CAPTURE_AUTHORITY_CAPTURE_PIXELS);
	if (!ret) {
		ret = castkms_capture_buffer_submit(client->buffer,
						    &client->request);
		castkms_capture_authority_end(client->authority);
	}

	return ret;
}

static int castkms_core_client_get_completion_fence(
	struct castkms_core_client *client)
{
	struct drm_gem_object *obj =
		drm_gem_fb_get_obj(client->framebuffer, 0);

	if (WARN_ON(!obj || !obj->resv))
		return -EINVAL;
	dma_fence_put(client->completion_fence);
	client->completion_fence = NULL;
	return dma_resv_get_singleton(obj->resv, DMA_RESV_USAGE_WRITE,
				      &client->completion_fence);
}

static int castkms_core_client_test_init(struct kunit *test)
{
	const u32 rights = CASTKMS_CAPTURE_AUTHORITY_CAPTURE_PIXELS |
		CASTKMS_CAPTURE_AUTHORITY_MANAGE_ATTACHMENT;
	struct castkms_config_connector *connector_config;
	struct castkms_config_crtc *crtc_config;
	struct castkms_core_client *client;
	struct castkms_capture_authority *authority;
	struct drm_connector *connector;
	int ret;

	client = kunit_kzalloc(test, sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;
	test->priv = client;
	ret = kunit_add_action_or_reset(test, castkms_core_client_cleanup,
					client);
	if (ret)
		return ret;

	client->config = castkms_config_default_create(false, false, false,
						       false);
	if (IS_ERR(client->config)) {
		ret = PTR_ERR(client->config);
		client->config = NULL;
		return ret;
	}
	ret = castkms_create(client->config);
	if (ret)
		return ret;

	crtc_config = list_first_entry(&client->config->crtcs,
				       struct castkms_config_crtc, link);
	connector_config = list_first_entry(&client->config->connectors,
					    struct castkms_config_connector, link);
	client->output = crtc_config->crtc;
	client->connector = connector_config->connector;
	if (!client->output || !client->connector)
		return -ENODEV;
	connector = &client->connector->base;
	client->master = kunit_kzalloc(test, sizeof(*client->master), GFP_KERNEL);
	if (!client->master)
		return -ENOMEM;
	castkms_core_client_activate_output(client);

	client->framebuffer =
		castkms_core_client_create_framebuffer(&client->config->dev->drm);
	if (IS_ERR(client->framebuffer)) {
		ret = PTR_ERR(client->framebuffer);
		client->framebuffer = NULL;
		return ret;
	}

	authority = castkms_capture_authority_create(
		client->config->dev, connector, NULL, rights, true,
		&castkms_core_client_authority_ops, client);
	if (IS_ERR(authority))
		return PTR_ERR(authority);
	client->authority = authority;

	ret = castkms_capture_authority_begin(
		authority, connector,
		CASTKMS_CAPTURE_AUTHORITY_MANAGE_ATTACHMENT);
	if (!ret) {
		mutex_lock(&client->config->dev->attach_transition_lock);
		ret = castkms_connector_attach_monitor(connector, authority, NULL);
		mutex_unlock(&client->config->dev->attach_transition_lock);
		castkms_capture_authority_end(authority);
	}

	return ret;
}

static void castkms_core_client_request_revocation(struct kunit *test)
{
	struct castkms_core_client *client = test->priv;
	struct castkms_capture_authority *authority = client->authority;

	KUNIT_ASSERT_EQ(test, castkms_core_client_start_stream(client), 0);
	KUNIT_ASSERT_EQ(test, castkms_core_client_create_buffer(client), 0);
	KUNIT_ASSERT_EQ(test, castkms_core_client_submit(client), 0);
	KUNIT_EXPECT_EQ(test, castkms_core_client_submit(client), -EBUSY);
	KUNIT_EXPECT_EQ(test,
		castkms_capture_buffer_remove(client->stream, client->buffer),
		-EBUSY);

	castkms_capture_authority_revoke(authority, -EKEYREVOKED);
	KUNIT_EXPECT_FALSE(test, castkms_connector_authority_is_attached(
		&client->connector->base, authority));
	KUNIT_EXPECT_EQ(test, client->stop_calls, 1U);
	KUNIT_EXPECT_EQ(test, client->stop_status, -EKEYREVOKED);
	KUNIT_EXPECT_EQ(test, client->request_calls, 1U);
	KUNIT_EXPECT_EQ(test, client->result.status, -EKEYREVOKED);
	KUNIT_EXPECT_FALSE(test, client->result.cancelled);
	KUNIT_EXPECT_EQ(test, client->revoke_calls, 1U);
	KUNIT_EXPECT_EQ(test, client->revoke_status, -EKEYREVOKED);
	KUNIT_EXPECT_PTR_EQ(test, client->stream, NULL);

	client->authority = NULL;
	castkms_capture_authority_put(authority);
	KUNIT_EXPECT_EQ(test, client->release_calls, 1U);
}

static void castkms_core_client_mode_change_cancellation(struct kunit *test)
{
	struct castkms_core_client *client = test->priv;
	struct castkms_capture_completion completion = {};
	struct drm_crtc_state state = {
		.active = true,
	};
	bool cancelled;

	KUNIT_ASSERT_EQ(test, castkms_core_client_start_stream(client), 0);
	KUNIT_ASSERT_EQ(test, castkms_core_client_create_buffer(client), 0);
	KUNIT_ASSERT_EQ(test, castkms_core_client_submit(client), 0);
	KUNIT_ASSERT_EQ(test,
		castkms_core_client_get_completion_fence(client), 0);
	KUNIT_ASSERT_NOT_NULL(test, client->completion_fence);
	KUNIT_EXPECT_FALSE(test,
		dma_fence_is_signaled(client->completion_fence));

	state.mode.hdisplay = CORE_CLIENT_WIDTH;
	state.mode.vdisplay = CORE_CLIENT_HEIGHT;
	cancelled = castkms_capture_mode_changed(client->output, &state,
						 &completion);
	KUNIT_ASSERT_TRUE(test, cancelled);
	castkms_frame_dispatch_put(client->output,
			     CASTKMS_FRAME_DISPATCH_CLIENT_CAPTURE);
	castkms_capture_deliver_completion(client->output, &completion);

	KUNIT_EXPECT_EQ(test, client->request_calls, 1U);
	KUNIT_EXPECT_EQ(test, client->result.status, -ESTALE);
	KUNIT_EXPECT_FALSE(test,
		client->callback_saw_completion_fence_signaled);
	KUNIT_EXPECT_TRUE(test,
		dma_fence_is_signaled(client->completion_fence));
	KUNIT_EXPECT_EQ(test, dma_fence_get_status(client->completion_fence),
			-ESTALE);
	KUNIT_EXPECT_FALSE(test, client->result.cancelled);
	KUNIT_EXPECT_TRUE(test, client->result.mode_changed);
	KUNIT_EXPECT_EQ(test,
		castkms_capture_buffer_remove(client->stream, client->buffer), 0);
	client->buffer = NULL;
	castkms_core_client_stop_stream(client, -ECANCELED);
	KUNIT_EXPECT_EQ(test, client->stop_calls, 0U);
}

static void castkms_core_client_master_drop_cleans_stale_stream(
	struct kunit *test)
{
	struct castkms_core_client *client = test->priv;
	struct castkms_capture_owner_state *owners =
		&client->config->dev->capture_owners;
	u64 cleanup_sequence;
	u64 current_cleanup_sequence;
	unsigned long flags;

	KUNIT_ASSERT_EQ(test, castkms_core_client_start_stream(client), 0);
	KUNIT_ASSERT_EQ(test, castkms_core_client_create_buffer(client), 0);
	KUNIT_ASSERT_EQ(test, castkms_core_client_submit(client), 0);

	spin_lock_irqsave(&owners->lock, flags);
	cleanup_sequence = owners->cleanup_sequence;
	spin_unlock_irqrestore(&owners->lock, flags);
	castkms_capture_owner_master_drop(&client->config->dev->drm,
					  &client->owner_file);
	flush_work(&owners->work);

	spin_lock_irqsave(&owners->lock, flags);
	current_cleanup_sequence = owners->cleanup_sequence;
	spin_unlock_irqrestore(&owners->lock, flags);
	KUNIT_EXPECT_EQ(test, current_cleanup_sequence, cleanup_sequence + 1);
	KUNIT_EXPECT_EQ(test, client->stop_calls, 1U);
	KUNIT_EXPECT_EQ(test, client->stop_status, -EAGAIN);
	KUNIT_EXPECT_EQ(test, client->request_calls, 1U);
	KUNIT_EXPECT_EQ(test, client->result.status, -EAGAIN);
	KUNIT_EXPECT_FALSE(test, client->result.cancelled);
	KUNIT_EXPECT_FALSE(test,
		castkms_capture_authority_is_revoked(client->authority));
	KUNIT_EXPECT_EQ(test, client->revoke_calls, 0U);
	KUNIT_EXPECT_PTR_EQ(test, client->stream, NULL);
}

static void castkms_core_client_file_close_cleans_stale_stream(
	struct kunit *test)
{
	struct castkms_core_client *client = test->priv;
	struct castkms_capture_owner_state *owners =
		&client->config->dev->capture_owners;
	struct drm_file *master_file;
	u64 cleanup_sequence;
	u64 current_cleanup_sequence;
	unsigned long flags;
	bool master_active;

	KUNIT_ASSERT_EQ(test, castkms_core_client_start_stream(client), 0);
	KUNIT_ASSERT_EQ(test, castkms_core_client_create_buffer(client), 0);
	KUNIT_ASSERT_EQ(test, castkms_core_client_submit(client), 0);

	spin_lock_irqsave(&owners->lock, flags);
	cleanup_sequence = owners->cleanup_sequence;
	spin_unlock_irqrestore(&owners->lock, flags);
	castkms_capture_owner_file_close(&client->config->dev->drm,
					&client->owner_file);
	/* A repeated close must not advance or re-notify the same epoch. */
	castkms_capture_owner_file_close(&client->config->dev->drm,
					&client->owner_file);
	flush_work(&owners->work);

	spin_lock_irqsave(&owners->lock, flags);
	current_cleanup_sequence = owners->cleanup_sequence;
	master_file = owners->master_file;
	master_active = owners->master_active;
	spin_unlock_irqrestore(&owners->lock, flags);
	KUNIT_EXPECT_EQ(test, current_cleanup_sequence, cleanup_sequence + 1);
	KUNIT_EXPECT_PTR_EQ(test, master_file, NULL);
	KUNIT_EXPECT_FALSE(test, master_active);
	KUNIT_EXPECT_EQ(test, client->stop_calls, 1U);
	KUNIT_EXPECT_EQ(test, client->stop_status, -EAGAIN);
	KUNIT_EXPECT_EQ(test, client->request_calls, 1U);
	KUNIT_EXPECT_EQ(test, client->result.status, -EAGAIN);
	KUNIT_EXPECT_FALSE(test,
		castkms_capture_authority_is_revoked(client->authority));
	KUNIT_EXPECT_EQ(test, client->revoke_calls, 0U);
	KUNIT_EXPECT_PTR_EQ(test, client->stream, NULL);
}

static struct kunit_case castkms_core_client_test_cases[] = {
	KUNIT_CASE(castkms_core_client_request_revocation),
	KUNIT_CASE(castkms_core_client_mode_change_cancellation),
	KUNIT_CASE(castkms_core_client_master_drop_cleans_stale_stream),
	KUNIT_CASE(castkms_core_client_file_close_cleans_stale_stream),
	{}
};

static struct kunit_suite castkms_core_client_test_suite = {
	.name = "castkms-core-client",
	.init = castkms_core_client_test_init,
	.test_cases = castkms_core_client_test_cases,
};

kunit_test_suite(castkms_core_client_test_suite);

MODULE_DESCRIPTION("KUnit client for the transport-neutral CastKMS capture core");
MODULE_LICENSE("GPL");
