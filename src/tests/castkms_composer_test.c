// SPDX-License-Identifier: GPL-2.0+

#include <kunit/test.h>

#include "../castkms_composer.h"

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

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
