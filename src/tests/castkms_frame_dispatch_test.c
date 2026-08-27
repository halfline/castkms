// SPDX-License-Identifier: GPL-2.0+

#include <kunit/test.h>

#include "../castkms_crtc.h"
#include "../castkms_frame_dispatch.h"

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

static void castkms_frame_dispatch_test_sorts_planes_by_zpos(struct kunit *test)
{
	struct castkms_frame_plane primary = {};
	struct castkms_frame_plane overlay = {};
	struct castkms_frame_plane cursor = {};
	struct castkms_frame_plane *planes[] = {
		&cursor,
		&primary,
		&overlay,
	};

	primary.zpos = 0;
	overlay.zpos = 7;
	cursor.zpos = overlay.zpos + 1;

	castkms_sort_frame_planes(planes, ARRAY_SIZE(planes));

	KUNIT_EXPECT_PTR_EQ(test, planes[0], &primary);
	KUNIT_EXPECT_PTR_EQ(test, planes[1], &overlay);
	KUNIT_EXPECT_PTR_EQ(test, planes[2], &cursor);
}

static struct kunit_case castkms_frame_dispatch_test_cases[] = {
	KUNIT_CASE(castkms_frame_dispatch_test_sorts_planes_by_zpos),
	{}
};

static struct kunit_suite castkms_frame_dispatch_test_suite = {
	.name = "castkms-frame-dispatch",
	.test_cases = castkms_frame_dispatch_test_cases,
};

kunit_test_suite(castkms_frame_dispatch_test_suite);

MODULE_DESCRIPTION("KUnit tests for CASTKMS composition state");
MODULE_LICENSE("GPL");
