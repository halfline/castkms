// SPDX-License-Identifier: GPL-2.0-only

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/kref.h>
#include <linux/kthread.h>

#include <kunit/test.h>

#include <drm/drm_auth.h>

#include <media/cec.h>

#include "../castkms_capture_authority.h"
#include "../castkms_cec_core.h"
#include "../castkms_config.h"
#include "../castkms_connector.h"
#include "../castkms_drv.h"
#include "../castkms_output.h"

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

struct castkms_cec_test_context {
	struct castkms_config *config;
	struct castkms_output *output;
	struct castkms_connector *connector;
	struct drm_master *master;
	struct castkms_capture_authority *authority;
	struct castkms_cec_output *cec;
	struct castkms_cec_state bound_state;

	struct castkms_cec_request request;
	struct castkms_cec_tx tx;
	struct completion prepare_entered;
	struct completion prepare_continue;
	struct completion transmit_done;
	struct completion revoke_started;
	struct completion revoke_done;
	struct task_struct *transmit_task;
	struct task_struct *revoke_task;
	bool block_prepare;
	int prepare_status;
	int transmit_status;

	atomic_t request_completions;
	atomic_t request_cancellations;
	atomic_t retirements;
	atomic_t releases;
	atomic_t tx_notifications;
	u8 notified_status;
};

static void castkms_cec_test_request_complete(
	struct castkms_cec_request *request, bool cancelled)
{
	struct castkms_cec_test_context *context =
		container_of(request, struct castkms_cec_test_context, request);

	atomic_inc(&context->request_completions);
	if (cancelled)
		atomic_inc(&context->request_cancellations);
}

static void castkms_cec_test_request_retire(void *data)
{
	struct castkms_cec_test_context *context = data;

	atomic_inc(&context->retirements);
}

static int castkms_cec_test_prepare_tx(void *data,
				       const struct castkms_cec_tx *tx,
				       struct castkms_cec_request **request)
{
	struct castkms_cec_test_context *context = data;

	context->tx = *tx;
	if (context->block_prepare) {
		complete(&context->prepare_entered);
		wait_for_completion(&context->prepare_continue);
	}
	if (context->prepare_status)
		return context->prepare_status;

	context->request.complete = castkms_cec_test_request_complete;
	context->request.retire = castkms_cec_test_request_retire;
	context->request.retire_data = context;
	*request = &context->request;
	return 0;
}

static void castkms_cec_test_release(void *data)
{
	struct castkms_cec_test_context *context = data;

	atomic_inc(&context->releases);
}

static const struct castkms_cec_transport_ops castkms_cec_test_transport_ops = {
	.prepare_tx = castkms_cec_test_prepare_tx,
	.release = castkms_cec_test_release,
};

static void castkms_cec_test_tx_done(void *data, u8 status,
				     u8 arb_lost_cnt, u8 nack_cnt,
				     u8 low_drive_cnt, u8 error_cnt)
{
	struct castkms_cec_test_context *context = data;

	(void)arb_lost_cnt;
	(void)nack_cnt;
	(void)low_drive_cnt;
	(void)error_cnt;
	context->notified_status = status;
	atomic_inc(&context->tx_notifications);
}

static const struct castkms_cec_test_ops castkms_cec_test_sink_ops = {
	.tx_done = castkms_cec_test_tx_done,
};

static void castkms_cec_test_activate_output(
	struct castkms_cec_test_context *context)
{
	struct castkms_capture_owner_state *owners =
		&context->config->dev->capture_owners;
	unsigned long flags;

	context->master->dev = &context->config->dev->drm;
	kref_init(&context->master->refcount);

	spin_lock_irqsave(&owners->lock, flags);
	owners->master = drm_master_get(context->master);
	owners->master_active = true;
	spin_unlock_irqrestore(&owners->lock, flags);

	spin_lock_irqsave(&context->output->lock, flags);
	context->output->capture_owner = drm_master_get(context->master);
	context->output->capture_owner_updating = false;
	spin_unlock_irqrestore(&context->output->lock, flags);

	spin_lock_irqsave(&context->output->capture.state_lock, flags);
	context->output->capture.width = 640;
	context->output->capture.height = 480;
	context->output->capture.active = true;
	spin_unlock_irqrestore(&context->output->capture.state_lock, flags);
}

static int castkms_cec_test_begin(
	struct castkms_cec_test_context *context, u32 rights)
{
	return castkms_capture_authority_begin(context->authority,
					       &context->connector->base, rights);
}

static int castkms_cec_test_get_state(
	struct castkms_cec_test_context *context,
	struct castkms_cec_state *state)
{
	int ret;

	ret = castkms_cec_test_begin(context,
				     CASTKMS_CAPTURE_AUTHORITY_MANAGE_CEC);
	if (ret)
		return ret;
	ret = castkms_cec_core_get_state(context->cec, context->authority,
					 state);
	castkms_capture_authority_end(context->authority);
	return ret;
}

static int castkms_cec_test_complete_tx(
	struct castkms_cec_test_context *context, u64 cookie, u8 status)
{
	int ret;

	ret = castkms_cec_test_begin(context,
				     CASTKMS_CAPTURE_AUTHORITY_MANAGE_CEC);
	if (ret)
		return ret;
	ret = castkms_cec_core_tx_complete(
		context->cec, context->authority,
		context->bound_state.transport_generation, cookie, status,
		0, 0, 0, status & CEC_TX_STATUS_ERROR ? 1 : 0);
	castkms_capture_authority_end(context->authority);
	return ret;
}

static void castkms_cec_test_cleanup(void *data)
{
	struct castkms_cec_test_context *context = data;
	unsigned long flags;

	if (context->authority) {
		castkms_capture_authority_revoke(context->authority, -ECANCELED);
		castkms_capture_authority_put(context->authority);
		context->authority = NULL;
	}
	if (context->cec) {
		castkms_cec_core_test_output_destroy(context->cec);
		context->cec = NULL;
	}
	if (context->output) {
		spin_lock_irqsave(&context->output->capture.state_lock, flags);
		context->output->capture.active = false;
		context->output->capture.width = 0;
		context->output->capture.height = 0;
		spin_unlock_irqrestore(&context->output->capture.state_lock, flags);
	}
	if (context->config && context->config->dev)
		castkms_destroy(context->config);
	if (context->config) {
		castkms_config_destroy(context->config);
		context->config = NULL;
	}
}

static int castkms_cec_test_init(struct kunit *test)
{
	const u32 rights = CASTKMS_CAPTURE_AUTHORITY_MANAGE_ATTACHMENT |
		CASTKMS_CAPTURE_AUTHORITY_MANAGE_CEC;
	struct castkms_config_connector *connector_config;
	struct castkms_config_crtc *crtc_config;
	struct castkms_cec_test_context *context;
	int ret;

	context = kunit_kzalloc(test, sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;
	test->priv = context;
	init_completion(&context->prepare_entered);
	init_completion(&context->prepare_continue);
	init_completion(&context->transmit_done);
	init_completion(&context->revoke_started);
	init_completion(&context->revoke_done);
	ret = kunit_add_action_or_reset(test, castkms_cec_test_cleanup, context);
	if (ret)
		return ret;

	context->config = castkms_config_default_create(false, false, false,
							false);
	if (IS_ERR(context->config)) {
		ret = PTR_ERR(context->config);
		context->config = NULL;
		return ret;
	}
	ret = castkms_create(context->config);
	if (ret)
		return ret;

	crtc_config = list_first_entry(&context->config->crtcs,
				       struct castkms_config_crtc, link);
	connector_config = list_first_entry(&context->config->connectors,
					    struct castkms_config_connector, link);
	context->output = crtc_config->crtc;
	context->connector = connector_config->connector;
	if (!context->output || !context->connector)
		return -ENODEV;

	context->cec = castkms_cec_core_test_output_create(
		context->connector, &castkms_cec_test_sink_ops, context);
	if (IS_ERR(context->cec)) {
		ret = PTR_ERR(context->cec);
		context->cec = NULL;
		return ret;
	}

	context->master = kunit_kzalloc(test, sizeof(*context->master),
					GFP_KERNEL);
	if (!context->master)
		return -ENOMEM;
	castkms_cec_test_activate_output(context);

	context->authority = castkms_capture_authority_create(
		context->config->dev, &context->connector->base, context->master,
		rights, false, NULL, NULL);
	if (IS_ERR(context->authority)) {
		ret = PTR_ERR(context->authority);
		context->authority = NULL;
		return ret;
	}

	ret = castkms_cec_test_begin(
		context, CASTKMS_CAPTURE_AUTHORITY_MANAGE_ATTACHMENT);
	if (ret)
		return ret;
	mutex_lock(&context->config->dev->attach_transition_lock);
	ret = castkms_connector_attach_monitor(&context->connector->base,
					       context->authority, NULL);
	mutex_unlock(&context->config->dev->attach_transition_lock);
	castkms_capture_authority_end(context->authority);
	if (ret)
		return ret;

	ret = castkms_cec_test_begin(context,
				     CASTKMS_CAPTURE_AUTHORITY_MANAGE_CEC);
	if (ret)
		return ret;
	ret = castkms_cec_core_bind(context->cec, context->authority,
				    &castkms_cec_test_transport_ops, context,
				    &context->bound_state);
	castkms_capture_authority_end(context->authority);
	if (ret)
		return ret;

	ret = castkms_cec_test_begin(context,
				     CASTKMS_CAPTURE_AUTHORITY_MANAGE_CEC);
	if (ret)
		return ret;
	ret = castkms_cec_core_set_online(
		context->cec, context->authority,
		context->bound_state.transport_generation, true);
	castkms_capture_authority_end(context->authority);
	if (ret)
		return ret;

	return castkms_cec_core_test_enable(context->cec, true);
}

static void castkms_cec_test_completion_beats_timeout(struct kunit *test)
{
	static const u8 message[] = { 0x4f, CEC_MSG_REQUEST_ACTIVE_SOURCE };
	struct castkms_cec_test_context *context = test->priv;
	struct castkms_cec_state state;
	int ret;

	ret = castkms_cec_core_test_transmit(context->cec, 2, 5, message,
					     sizeof(message));
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&context->request_completions), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&context->request_cancellations), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&context->retirements), 1);
	KUNIT_EXPECT_EQ(test, context->tx.attempts, (u8)2);
	KUNIT_EXPECT_EQ(test, context->tx.signal_free_time, (u32)5);

	ret = castkms_cec_test_complete_tx(context, context->tx.cookie,
					   CEC_TX_STATUS_OK);
	KUNIT_ASSERT_EQ(test, ret, 0);
	castkms_cec_core_test_timeout(context->cec);
	KUNIT_ASSERT_EQ(test, castkms_cec_test_get_state(context, &state), 0);
	KUNIT_EXPECT_EQ(test, state.pending_cookie, (u64)0);
	KUNIT_EXPECT_EQ(test, state.stats_tx_submitted, (u64)1);
	KUNIT_EXPECT_EQ(test, state.stats_tx_completed, (u64)1);
	KUNIT_EXPECT_EQ(test, state.stats_tx_timeout, (u64)0);
	KUNIT_EXPECT_EQ(test, atomic_read(&context->tx_notifications), 1);
	KUNIT_EXPECT_EQ(test, context->notified_status, (u8)CEC_TX_STATUS_OK);
}

static void castkms_cec_test_timeout_rejects_late_completion(struct kunit *test)
{
	static const u8 message[] = { 0x4f, CEC_MSG_REQUEST_ACTIVE_SOURCE };
	struct castkms_cec_test_context *context = test->priv;
	struct castkms_cec_state state;
	int ret;

	ret = castkms_cec_core_test_transmit(context->cec, 1, 0, message,
					     sizeof(message));
	KUNIT_ASSERT_EQ(test, ret, 0);
	castkms_cec_core_test_timeout(context->cec);
	ret = castkms_cec_test_complete_tx(context, context->tx.cookie,
					   CEC_TX_STATUS_OK);
	KUNIT_EXPECT_EQ(test, ret, -ENOENT);
	KUNIT_ASSERT_EQ(test, castkms_cec_test_get_state(context, &state), 0);
	KUNIT_EXPECT_EQ(test, state.pending_cookie, (u64)0);
	KUNIT_EXPECT_EQ(test, state.stats_tx_timeout, (u64)1);
	KUNIT_EXPECT_EQ(test, state.stats_tx_completed, (u64)0);
	KUNIT_EXPECT_EQ(test, state.stats_invalid, (u64)1);
	KUNIT_EXPECT_EQ(test, atomic_read(&context->tx_notifications), 1);
	KUNIT_EXPECT_EQ(test, context->notified_status,
			(u8)CEC_TX_STATUS_ERROR);
}

static struct kunit_case castkms_cec_test_cases[] = {
	KUNIT_CASE(castkms_cec_test_completion_beats_timeout),
	KUNIT_CASE(castkms_cec_test_timeout_rejects_late_completion),
	{}
};

static struct kunit_suite castkms_cec_test_suite = {
	.name = "castkms-cec-core",
	.init = castkms_cec_test_init,
	.test_cases = castkms_cec_test_cases,
};

kunit_test_suite(castkms_cec_test_suite);

MODULE_DESCRIPTION("KUnit tests for CastKMS CEC core lifecycle races");
MODULE_LICENSE("GPL");
