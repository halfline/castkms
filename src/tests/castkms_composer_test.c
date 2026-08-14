// SPDX-License-Identifier: GPL-2.0+

#include <kunit/test.h>

#include "../castkms_composer.h"

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

static void castkms_composer_test_crc_is_idempotent(struct kunit *test)
{
	struct castkms_composer_demand demand = {};
	bool put_vblank;
	bool keep_vblank;
	int ret;

	ret = castkms_composer_demand_get(&demand,
					  CASTKMS_COMPOSER_CLIENT_CRC,
					  -EINVAL, &keep_vblank);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_FALSE(test, keep_vblank);
	KUNIT_EXPECT_FALSE(test, castkms_composer_demand_is_active(&demand));

	ret = castkms_composer_demand_get(&demand,
					  CASTKMS_COMPOSER_CLIENT_CRC,
					  0, &keep_vblank);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, keep_vblank);
	KUNIT_EXPECT_TRUE(test, demand.crc_enabled);

	ret = castkms_composer_demand_get(&demand,
					  CASTKMS_COMPOSER_CLIENT_CRC,
					  0, &keep_vblank);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, keep_vblank);

	ret = castkms_composer_demand_get(&demand,
					  CASTKMS_COMPOSER_CLIENT_CRC,
					  -EINVAL, &keep_vblank);
	KUNIT_ASSERT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_FALSE(test, keep_vblank);
	KUNIT_EXPECT_TRUE(test, demand.crc_enabled);

	put_vblank = castkms_composer_demand_put(&demand,
						 CASTKMS_COMPOSER_CLIENT_CRC);
	KUNIT_EXPECT_TRUE(test, put_vblank);
	put_vblank = castkms_composer_demand_put(&demand,
						 CASTKMS_COMPOSER_CLIENT_CRC);
	KUNIT_EXPECT_FALSE(test, put_vblank);
}

static void castkms_composer_test_writebacks_are_counted(struct kunit *test)
{
	struct castkms_composer_demand demand = {};
	bool put_vblank;
	bool keep_vblank;
	int ret;

	ret = castkms_composer_demand_get(&demand,
					  CASTKMS_COMPOSER_CLIENT_CRC,
					  0, &keep_vblank);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_TRUE(test, keep_vblank);

	ret = castkms_composer_demand_get(&demand,
					  CASTKMS_COMPOSER_CLIENT_WRITEBACK,
					  0, &keep_vblank);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, keep_vblank);

	ret = castkms_composer_demand_get(&demand,
					  CASTKMS_COMPOSER_CLIENT_WRITEBACK,
					  0, &keep_vblank);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, keep_vblank);
	KUNIT_EXPECT_EQ(test, demand.writeback_count, 2);
	ret = castkms_composer_demand_get(&demand,
					  CASTKMS_COMPOSER_CLIENT_WRITEBACK,
					  -EINVAL, &keep_vblank);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_EQ(test, demand.writeback_count, 2);

	put_vblank = castkms_composer_demand_put(&demand,
						 CASTKMS_COMPOSER_CLIENT_WRITEBACK);
	KUNIT_EXPECT_FALSE(test, put_vblank);
	KUNIT_EXPECT_EQ(test, demand.writeback_count, 1);
	put_vblank = castkms_composer_demand_put(&demand,
						 CASTKMS_COMPOSER_CLIENT_CRC);
	KUNIT_EXPECT_FALSE(test, put_vblank);
	put_vblank = castkms_composer_demand_put(&demand,
						 CASTKMS_COMPOSER_CLIENT_WRITEBACK);
	KUNIT_EXPECT_TRUE(test, put_vblank);
	KUNIT_EXPECT_FALSE(test, castkms_composer_demand_is_active(&demand));
}

static void castkms_composer_test_failed_first_writeback(struct kunit *test)
{
	struct castkms_composer_demand demand = {};
	bool keep_vblank;
	int ret;

	ret = castkms_composer_demand_get(&demand,
					  CASTKMS_COMPOSER_CLIENT_WRITEBACK,
					  -EINVAL, &keep_vblank);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_FALSE(test, keep_vblank);
	KUNIT_EXPECT_EQ(test, demand.writeback_count, 0);
	KUNIT_EXPECT_FALSE(test, castkms_composer_demand_is_active(&demand));
}

static void castkms_composer_test_sorts_planes_by_zpos(struct kunit *test)
{
	struct castkms_plane_state primary = {};
	struct castkms_plane_state overlay = {};
	struct castkms_plane_state cursor = {};
	struct castkms_plane_state *planes[] = {
		&cursor,
		&primary,
		&overlay,
	};

	primary.base.base.normalized_zpos = 0;
	overlay.base.base.normalized_zpos = 7;
	cursor.base.base.normalized_zpos = CASTKMS_MAX_OUTPUT_OBJECTS;

	castkms_sort_plane_states(planes, ARRAY_SIZE(planes));

	KUNIT_EXPECT_PTR_EQ(test, planes[0], &primary);
	KUNIT_EXPECT_PTR_EQ(test, planes[1], &overlay);
	KUNIT_EXPECT_PTR_EQ(test, planes[2], &cursor);
}

static struct kunit_case castkms_composer_test_cases[] = {
	KUNIT_CASE(castkms_composer_test_crc_is_idempotent),
	KUNIT_CASE(castkms_composer_test_writebacks_are_counted),
	KUNIT_CASE(castkms_composer_test_failed_first_writeback),
	KUNIT_CASE(castkms_composer_test_sorts_planes_by_zpos),
	{}
};

static struct kunit_suite castkms_composer_test_suite = {
	.name = "castkms-composer",
	.test_cases = castkms_composer_test_cases,
};

kunit_test_suite(castkms_composer_test_suite);

MODULE_DESCRIPTION("KUnit tests for CASTKMS composition state");
MODULE_LICENSE("GPL");
