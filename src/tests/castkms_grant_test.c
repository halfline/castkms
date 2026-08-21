// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>

#include <drm/castkms_drm.h>
#include <drm/drm_auth.h>

#include "../castkms_capture_authority.h"
#include "../castkms_grant.h"

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

static enum castkms_capture_authority_state
authority_state(bool revoked, bool shutdown, bool administrative,
		const void *bound_master, const void *current_master,
		bool master_active, bool connector_ready, bool content_safe)
{
	return castkms_capture_authority_resolve_state(
		revoked, shutdown, administrative, bound_master,
		current_master, master_active, connector_ready, content_safe);
}

static void castkms_grant_pending_before_safe_output(struct kunit *test)
{
	static const int master_a;

	KUNIT_EXPECT_EQ(test,
		authority_state(false, false, false, &master_a, &master_a, true,
			    false, false),
		CASTKMS_CAPTURE_AUTHORITY_PENDING);
}

static void castkms_grant_active_for_owned_content(struct kunit *test)
{
	static const int master_a;

	KUNIT_EXPECT_EQ(test,
		authority_state(false, false, false, &master_a, &master_a, true,
			    true, true),
		CASTKMS_CAPTURE_AUTHORITY_ACTIVE);
}

static void castkms_grant_suspends_without_master(struct kunit *test)
{
	static const int master_a;

	KUNIT_EXPECT_EQ(test,
		authority_state(false, false, false, &master_a, NULL, false,
			    true, true),
		CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_NO_MASTER);
}

static void castkms_grant_suspends_for_other_master(struct kunit *test)
{
	static const int master_a;
	static const int master_b;

	KUNIT_EXPECT_EQ(test,
		authority_state(false, false, false, &master_a, &master_b, true,
			    true, true),
		CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_OTHER_MASTER);
}

static void castkms_grant_revivifies_after_intervening_master(struct kunit *test)
{
	static const int master_a;

	KUNIT_EXPECT_EQ(test,
		authority_state(false, false, false, &master_a, &master_a, true,
			    true, true),
		CASTKMS_CAPTURE_AUTHORITY_ACTIVE);
}

static void castkms_grant_waits_for_returning_owner_content(struct kunit *test)
{
	static const int master_a;

	KUNIT_EXPECT_EQ(test,
		authority_state(false, false, false, &master_a, &master_a, true,
			    true, false),
		CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_FOREIGN_CONTENT);
}

static void castkms_admin_grant_follows_current_safe_owner(struct kunit *test)
{
	static const int master_a;
	static const int master_b;

	KUNIT_EXPECT_EQ(test,
		authority_state(false, false, true, &master_a, &master_b, true,
			    true, true),
		CASTKMS_CAPTURE_AUTHORITY_ACTIVE);
}

static void castkms_admin_grant_waits_without_current_master(struct kunit *test)
{
	static const int master_a;

	KUNIT_EXPECT_EQ(test,
		authority_state(false, false, true, &master_a, NULL, false,
			    true, false),
		CASTKMS_CAPTURE_AUTHORITY_SUSPENDED_NO_MASTER);
}

static void castkms_admin_grant_blocks_foreign_residue(struct kunit *test)
{
	static const int master_a;
	static const int master_b;

	KUNIT_EXPECT_EQ(test,
		authority_state(false, false, true, &master_a, &master_b, true,
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

static void castkms_grant_rejects_lease_master(struct kunit *test)
{
	struct drm_master owner = {};
	struct drm_master lessee = {
		.lessor = &owner,
	};

	KUNIT_EXPECT_TRUE(test, castkms_grant_master_is_owner(&owner));
	KUNIT_EXPECT_FALSE(test, castkms_grant_master_is_owner(&lessee));
	KUNIT_EXPECT_FALSE(test, castkms_grant_master_is_owner(NULL));
}

static void castkms_grant_creation_policy(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test,
		castkms_grant_creation_status(0, false, true, true, true),
		0);
	KUNIT_EXPECT_EQ(test,
		castkms_grant_creation_status(0, true, false, false, true),
		-EACCES);
	KUNIT_EXPECT_EQ(test,
		castkms_grant_creation_status(0, true, true, false, true),
		-EACCES);

	KUNIT_EXPECT_EQ(test,
		castkms_grant_creation_status(
			DRM_CASTKMS_GRANT_CREATE_DELEGATED,
			true, false, false, true),
		0);
	KUNIT_EXPECT_EQ(test,
		castkms_grant_creation_status(
			DRM_CASTKMS_GRANT_CREATE_DELEGATED,
			false, false, false, true),
		-EACCES);
	KUNIT_EXPECT_EQ(test,
		castkms_grant_creation_status(
			DRM_CASTKMS_GRANT_CREATE_DELEGATED,
			true, true, true, true),
		-EAGAIN);
	KUNIT_EXPECT_EQ(test,
		castkms_grant_creation_status(
			DRM_CASTKMS_GRANT_CREATE_DELEGATED,
			true, false, false, false),
		-EAGAIN);

	KUNIT_EXPECT_EQ(test,
		castkms_grant_creation_status(DRM_CASTKMS_GRANT_CREATE_ADMIN,
					      true, false, false, false),
		0);
	KUNIT_EXPECT_EQ(test,
		castkms_grant_creation_status(DRM_CASTKMS_GRANT_CREATE_ADMIN,
					      false, false, false, true),
		-EACCES);
	KUNIT_EXPECT_EQ(test,
		castkms_grant_creation_status(
			DRM_CASTKMS_GRANT_CREATE_ADMIN |
			DRM_CASTKMS_GRANT_CREATE_DELEGATED,
			true, false, false, true),
		-EINVAL);
	KUNIT_EXPECT_EQ(test,
		castkms_grant_creation_status(BIT(31), true, false, false,
					      true),
		-EINVAL);
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

static void castkms_admin_stream_expires_after_master_cleanup(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test,
		castkms_capture_authority_stream_generation_is_current(6, 7));
	KUNIT_EXPECT_TRUE(test,
		castkms_capture_authority_stream_generation_is_current(7, 7));
}

static void castkms_grant_explicit_revoke_is_terminal(struct kunit *test)
{
	static const int master_a;

	KUNIT_EXPECT_EQ(test,
		authority_state(true, false, false, &master_a, &master_a, true,
			    true, true),
		CASTKMS_CAPTURE_AUTHORITY_REVOKED);
}

static void castkms_grant_device_shutdown_is_terminal(struct kunit *test)
{
	static const int master_a;

	KUNIT_EXPECT_EQ(test,
		authority_state(false, true, false, &master_a, &master_a, true,
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
	KUNIT_CASE(castkms_admin_grant_follows_current_safe_owner),
	KUNIT_CASE(castkms_admin_grant_waits_without_current_master),
	KUNIT_CASE(castkms_admin_grant_blocks_foreign_residue),
	KUNIT_CASE(castkms_capture_owner_requires_current_master),
	KUNIT_CASE(castkms_grant_rejects_lease_master),
	KUNIT_CASE(castkms_grant_creation_policy),
	KUNIT_CASE(castkms_master_cleanup_preserves_current_streams),
	KUNIT_CASE(castkms_admin_stream_expires_after_master_cleanup),
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
