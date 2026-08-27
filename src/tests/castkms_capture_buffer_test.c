// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>

#include "../castkms_capture_internal.h"

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

static void castkms_capture_buffer_transitions_are_exhaustive(
	struct kunit *test)
{
	static const bool allowed[CASTKMS_CAPTURE_BUFFER_STATE_COUNT]
				 [CASTKMS_CAPTURE_BUFFER_STATE_COUNT] = {
		[CASTKMS_CAPTURE_BUFFER_IDLE] = {
			[CASTKMS_CAPTURE_BUFFER_PREPARING] = true,
		},
		[CASTKMS_CAPTURE_BUFFER_PREPARING] = {
			[CASTKMS_CAPTURE_BUFFER_IDLE] = true,
			[CASTKMS_CAPTURE_BUFFER_WAITING_REUSE] = true,
			[CASTKMS_CAPTURE_BUFFER_QUEUED] = true,
			[CASTKMS_CAPTURE_BUFFER_COMPLETING] = true,
		},
		[CASTKMS_CAPTURE_BUFFER_WAITING_REUSE] = {
			[CASTKMS_CAPTURE_BUFFER_QUEUED] = true,
			[CASTKMS_CAPTURE_BUFFER_COMPLETING] = true,
		},
		[CASTKMS_CAPTURE_BUFFER_QUEUED] = {
			[CASTKMS_CAPTURE_BUFFER_IN_FLIGHT] = true,
			[CASTKMS_CAPTURE_BUFFER_COMPLETING] = true,
		},
		[CASTKMS_CAPTURE_BUFFER_IN_FLIGHT] = {
			[CASTKMS_CAPTURE_BUFFER_QUEUED] = true,
			[CASTKMS_CAPTURE_BUFFER_COMPLETING] = true,
		},
		[CASTKMS_CAPTURE_BUFFER_COMPLETING] = {
			[CASTKMS_CAPTURE_BUFFER_IDLE] = true,
		},
	};
	enum castkms_capture_buffer_state from;
	enum castkms_capture_buffer_state to;

	for (from = 0; from < CASTKMS_CAPTURE_BUFFER_STATE_COUNT; from++) {
		for (to = 0; to < CASTKMS_CAPTURE_BUFFER_STATE_COUNT; to++)
			KUNIT_EXPECT_EQ_MSG(
				test,
				castkms_capture_buffer_state_transition_valid(from,
								       to),
				allowed[from][to], "transition %u -> %u",
				(unsigned int)from, (unsigned int)to);
	}
}

static void castkms_capture_buffer_rejects_unknown_states(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test,
		castkms_capture_buffer_state_transition_valid(
			CASTKMS_CAPTURE_BUFFER_STATE_COUNT,
			CASTKMS_CAPTURE_BUFFER_IDLE));
	KUNIT_EXPECT_FALSE(test,
		castkms_capture_buffer_state_transition_valid(
			CASTKMS_CAPTURE_BUFFER_IDLE,
			CASTKMS_CAPTURE_BUFFER_STATE_COUNT));
}

static struct kunit_case castkms_capture_buffer_test_cases[] = {
	KUNIT_CASE(castkms_capture_buffer_transitions_are_exhaustive),
	KUNIT_CASE(castkms_capture_buffer_rejects_unknown_states),
	{}
};

static struct kunit_suite castkms_capture_buffer_test_suite = {
	.name = "castkms-capture-buffer",
	.test_cases = castkms_capture_buffer_test_cases,
};

kunit_test_suite(castkms_capture_buffer_test_suite);

MODULE_DESCRIPTION("KUnit tests for the CastKMS capture buffer state machine");
MODULE_LICENSE("GPL");
