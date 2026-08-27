// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>

#include "../castkms_capture_authority.h"
#include "../castkms_capture_owner.h"

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

static enum castkms_capture_authority_state
authority_state(bool revoked, bool shutdown, const void *bound_master,
		const void *current_master,
		bool master_active, bool connector_ready, bool content_safe)
{
	return castkms_capture_authority_resolve_state(
		revoked, shutdown, false, !!current_master,
		master_active, current_master && bound_master == current_master,
		connector_ready, content_safe);
}

static void castkms_grant_pending_before_safe_output(struct kunit *test)
{
	static const int master_a;

	KUNIT_EXPECT_EQ(test,
		authority_state(false, false, &master_a, &master_a, true,
			    false, false),
		CASTKMS_CAPTURE_AUTHORITY_PENDING);
}

static void castkms_grant_active_for_owned_content(struct kunit *test)
{
	static const int master_a;

	KUNIT_EXPECT_EQ(test,
		authority_state(false, false, &master_a, &master_a, true,
			    true, true),
		CASTKMS_CAPTURE_AUTHORITY_ACTIVE);
}

static void castkms_grant_suspends_without_master(struct kunit *test)
{
	static const int master_a;

	KUNIT_EXPECT_EQ(test,
		authority_state(false, false, &master_a, NULL, false,
			    true, true),
		CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_NO_MASTER);
}

static void castkms_grant_suspends_for_other_master(struct kunit *test)
{
	static const int master_a;
	static const int master_b;

	KUNIT_EXPECT_EQ(test,
		authority_state(false, false, &master_a, &master_b, true,
			    true, true),
		CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_OTHER_MASTER);
}

static void castkms_grant_revivifies_after_intervening_master(struct kunit *test)
{
	static const int master_a;

	KUNIT_EXPECT_EQ(test,
		authority_state(false, false, &master_a, &master_a, true,
			    true, true),
		CASTKMS_CAPTURE_AUTHORITY_ACTIVE);
}

static void castkms_grant_waits_for_returning_owner_content(struct kunit *test)
{
	static const int master_a;

	KUNIT_EXPECT_EQ(test,
		authority_state(false, false, &master_a, &master_a, true,
			    true, false),
		CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_FOREIGN_CONTENT);
}

static void castkms_capture_owner_requires_current_master(struct kunit *test)
{
	static const int master_a;
	static const int master_b;
	const struct drm_master *owner_a = (const void *)&master_a;
	const struct drm_master *owner_b = (const void *)&master_b;

	KUNIT_EXPECT_TRUE(test,
		castkms_capture_owner_is_current(owner_a, owner_a));
	KUNIT_EXPECT_FALSE(test,
		castkms_capture_owner_is_current(owner_a, owner_b));
	KUNIT_EXPECT_FALSE(test,
		castkms_capture_owner_is_current(owner_a, NULL));
	KUNIT_EXPECT_FALSE(test,
		castkms_capture_owner_is_current(NULL, owner_a));
}

static void castkms_master_cleanup_preserves_current_streams(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test,
		castkms_capture_authority_generation_is_stale(6, 7));
	KUNIT_EXPECT_FALSE(test,
		castkms_capture_authority_generation_is_stale(7, 7));
	KUNIT_EXPECT_FALSE(test,
		castkms_capture_authority_generation_is_stale(8, 7));
}

static void castkms_grant_explicit_revoke_is_terminal(struct kunit *test)
{
	static const int master_a;

	KUNIT_EXPECT_EQ(test,
		authority_state(true, false, &master_a, &master_a, true,
			    true, true),
		CASTKMS_CAPTURE_AUTHORITY_REVOKED);
}

static void castkms_grant_device_shutdown_is_terminal(struct kunit *test)
{
	static const int master_a;

	KUNIT_EXPECT_EQ(test,
		authority_state(false, true, &master_a, &master_a, true,
			    true, true),
		CASTKMS_CAPTURE_AUTHORITY_REVOKED);
}

static void castkms_blank_noop_does_not_claim_content(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test,
		castkms_capture_blank_establishes_owner(true, false, false,
							 false, false));
}

static void castkms_disabling_last_plane_claims_safe_blank(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test,
		castkms_capture_blank_establishes_owner(true, true, false,
							 false, false));
}

static struct kunit_case castkms_grant_test_cases[] = {
	KUNIT_CASE(castkms_grant_pending_before_safe_output),
	KUNIT_CASE(castkms_grant_active_for_owned_content),
	KUNIT_CASE(castkms_grant_suspends_without_master),
	KUNIT_CASE(castkms_grant_suspends_for_other_master),
	KUNIT_CASE(castkms_grant_revivifies_after_intervening_master),
	KUNIT_CASE(castkms_grant_waits_for_returning_owner_content),
	KUNIT_CASE(castkms_capture_owner_requires_current_master),
	KUNIT_CASE(castkms_master_cleanup_preserves_current_streams),
	KUNIT_CASE(castkms_grant_explicit_revoke_is_terminal),
	KUNIT_CASE(castkms_grant_device_shutdown_is_terminal),
	KUNIT_CASE(castkms_blank_noop_does_not_claim_content),
	KUNIT_CASE(castkms_disabling_last_plane_claims_safe_blank),
	{}
};

static struct kunit_suite castkms_grant_test_suite = {
	.name = "castkms-grant",
	.test_cases = castkms_grant_test_cases,
};

kunit_test_suite(castkms_grant_test_suite);

MODULE_DESCRIPTION("KUnit tests for CastKMS capture grant ownership");
MODULE_LICENSE("GPL");
