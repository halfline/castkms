// SPDX-License-Identifier: GPL-2.0-only

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "virtualscreen-edid.h"

static int tests_run;
static int tests_passed;

#define TEST(name)						\
	do {							\
		printf("%-50s ", #name);			\
		tests_run++;					\
	} while (0)

#define PASS()							\
	do {							\
		tests_passed++;					\
		printf("pass\n");				\
	} while (0)

#define FAIL(fmt, ...)						\
	do {							\
		printf("FAIL: " fmt "\n", ##__VA_ARGS__);	\
	} while (0)

static bool edid_block_checksum_valid(const uint8_t *block)
{
	unsigned int sum = 0;

	for (int i = 0; i < CASTKMS_EDID_BLOCK; i++)
		sum += block[i];
	return (sum & 0xff) == 0;
}

static void test_video_only_size(void)
{
	uint8_t edid[CASTKMS_EDID_BLOCK];
	int ret;

	TEST(video_only_size);
	ret = castkms_fill_edid(edid, sizeof(edid), NULL, 0);
	if (ret != CASTKMS_EDID_BLOCK) {
		FAIL("expected %d, got %d", CASTKMS_EDID_BLOCK, ret);
		return;
	}
	PASS();
}

static void test_audio_size(void)
{
	uint8_t edid[CASTKMS_EDID_BLOCK * 2];
	int ret;

	TEST(audio_size);
	ret = castkms_fill_edid(edid, sizeof(edid), NULL,
				CASTKMS_EDID_FLAG_AUDIO);
	if (ret != CASTKMS_EDID_BLOCK * 2) {
		FAIL("expected %d, got %d", CASTKMS_EDID_BLOCK * 2, ret);
		return;
	}
	PASS();
}

static void test_video_only_no_extensions(void)
{
	uint8_t edid[CASTKMS_EDID_BLOCK];

	TEST(video_only_no_extensions);
	castkms_fill_edid(edid, sizeof(edid), NULL, 0);
	if (edid[126] != 0) {
		FAIL("extension count %u, expected 0", edid[126]);
		return;
	}
	PASS();
}

static void test_audio_extension_count(void)
{
	uint8_t edid[CASTKMS_EDID_BLOCK * 2];

	TEST(audio_extension_count);
	castkms_fill_edid(edid, sizeof(edid), NULL, CASTKMS_EDID_FLAG_AUDIO);
	if (edid[126] != 1) {
		FAIL("extension count %u, expected 1", edid[126]);
		return;
	}
	PASS();
}

static void test_base_block_checksum(void)
{
	uint8_t edid[CASTKMS_EDID_BLOCK * 2];

	TEST(base_block_checksum);
	castkms_fill_edid(edid, sizeof(edid), NULL, CASTKMS_EDID_FLAG_AUDIO);
	if (!edid_block_checksum_valid(edid)) {
		FAIL("base block checksum invalid");
		return;
	}
	PASS();
}

static void test_cta_block_checksum(void)
{
	uint8_t edid[CASTKMS_EDID_BLOCK * 2];

	TEST(cta_block_checksum);
	castkms_fill_edid(edid, sizeof(edid), NULL, CASTKMS_EDID_FLAG_AUDIO);
	if (!edid_block_checksum_valid(&edid[CASTKMS_EDID_BLOCK])) {
		FAIL("CTA block checksum invalid");
		return;
	}
	PASS();
}

static void test_video_only_checksum(void)
{
	uint8_t edid[CASTKMS_EDID_BLOCK];

	TEST(video_only_checksum);
	castkms_fill_edid(edid, sizeof(edid), NULL, 0);
	if (!edid_block_checksum_valid(edid)) {
		FAIL("base block checksum invalid");
		return;
	}
	PASS();
}

static void test_cta_tag_and_revision(void)
{
	uint8_t edid[CASTKMS_EDID_BLOCK * 2];
	uint8_t *ext;

	TEST(cta_tag_and_revision);
	castkms_fill_edid(edid, sizeof(edid), NULL, CASTKMS_EDID_FLAG_AUDIO);
	ext = &edid[CASTKMS_EDID_BLOCK];
	if (ext[0] != 0x02 || ext[1] != 0x03) {
		FAIL("CTA tag=0x%02x rev=0x%02x, expected 0x02 0x03",
		     ext[0], ext[1]);
		return;
	}
	PASS();
}

static void test_cta_basic_audio_flag(void)
{
	uint8_t edid[CASTKMS_EDID_BLOCK * 2];
	uint8_t *ext;

	TEST(cta_basic_audio_flag);
	castkms_fill_edid(edid, sizeof(edid), NULL, CASTKMS_EDID_FLAG_AUDIO);
	ext = &edid[CASTKMS_EDID_BLOCK];
	if (!(ext[3] & 0x40)) {
		FAIL("basic audio bit not set in byte 3 (0x%02x)", ext[3]);
		return;
	}
	PASS();
}

static void test_cta_audio_data_block(void)
{
	uint8_t edid[CASTKMS_EDID_BLOCK * 2];
	uint8_t *ext;
	uint8_t tag, length;

	TEST(cta_audio_data_block);
	castkms_fill_edid(edid, sizeof(edid), NULL, CASTKMS_EDID_FLAG_AUDIO);
	ext = &edid[CASTKMS_EDID_BLOCK];

	tag = (ext[4] >> 5) & 0x07;
	length = ext[4] & 0x1f;
	if (tag != 1 || length != 3) {
		FAIL("audio data block tag=%u length=%u, expected 1/3",
		     tag, length);
		return;
	}
	PASS();
}

static void test_cta_lpcm_sad(void)
{
	uint8_t edid[CASTKMS_EDID_BLOCK * 2];
	uint8_t *ext;
	uint8_t format, channels, rates, bits;

	TEST(cta_lpcm_sad);
	castkms_fill_edid(edid, sizeof(edid), NULL, CASTKMS_EDID_FLAG_AUDIO);
	ext = &edid[CASTKMS_EDID_BLOCK];

	format = (ext[5] >> 3) & 0x0f;
	channels = (ext[5] & 0x07) + 1;
	rates = ext[6];
	bits = ext[7];

	if (format != 1) {
		FAIL("SAD format %u, expected 1 (LPCM)", format);
		return;
	}
	if (channels != 2) {
		FAIL("SAD channels %u, expected 2", channels);
		return;
	}
	if (!(rates & 0x01)) {
		FAIL("32 kHz not set (rates=0x%02x)", rates);
		return;
	}
	if (!(rates & 0x02)) {
		FAIL("44.1 kHz not set (rates=0x%02x)", rates);
		return;
	}
	if (!(rates & 0x04)) {
		FAIL("48 kHz not set (rates=0x%02x)", rates);
		return;
	}
	if (!(bits & 0x01)) {
		FAIL("16-bit not set (bits=0x%02x)", bits);
		return;
	}
	PASS();
}

static void test_cta_speaker_allocation(void)
{
	uint8_t edid[CASTKMS_EDID_BLOCK * 2];
	uint8_t *ext;
	uint8_t tag, length;

	TEST(cta_speaker_allocation);
	castkms_fill_edid(edid, sizeof(edid), NULL, CASTKMS_EDID_FLAG_AUDIO);
	ext = &edid[CASTKMS_EDID_BLOCK];

	tag = (ext[8] >> 5) & 0x07;
	length = ext[8] & 0x1f;
	if (tag != 4 || length != 3) {
		FAIL("speaker block tag=%u length=%u, expected 4/3",
		     tag, length);
		return;
	}
	if (ext[9] != 0x01) {
		FAIL("speaker allocation 0x%02x, expected 0x01 (FL/FR)",
		     ext[9]);
		return;
	}
	PASS();
}

static void test_cta_dtd_offset(void)
{
	uint8_t edid[CASTKMS_EDID_BLOCK * 2];
	uint8_t *ext;
	uint8_t dtd_offset;

	TEST(cta_dtd_offset);
	castkms_fill_edid(edid, sizeof(edid), NULL, CASTKMS_EDID_FLAG_AUDIO);
	ext = &edid[CASTKMS_EDID_BLOCK];
	dtd_offset = ext[2];
	if (dtd_offset != 12) {
		FAIL("DTD offset %u, expected 12", dtd_offset);
		return;
	}
	PASS();
}

static void test_named_edid(void)
{
	uint8_t edid[CASTKMS_EDID_BLOCK * 2];
	int ret;

	TEST(named_edid);
	ret = castkms_fill_edid(edid, sizeof(edid), "TestMonitor",
				CASTKMS_EDID_FLAG_AUDIO);
	if (ret != CASTKMS_EDID_BLOCK * 2) {
		FAIL("expected %d, got %d", CASTKMS_EDID_BLOCK * 2, ret);
		return;
	}
	if (!edid_block_checksum_valid(edid)) {
		FAIL("base block checksum invalid");
		return;
	}
	if (!edid_block_checksum_valid(&edid[CASTKMS_EDID_BLOCK])) {
		FAIL("CTA block checksum invalid");
		return;
	}
	PASS();
}

static void test_name_too_long(void)
{
	uint8_t edid[CASTKMS_EDID_BLOCK * 2];
	int ret;

	TEST(name_too_long);
	ret = castkms_fill_edid(edid, sizeof(edid), "ThisNameIsTooLong", 0);
	if (ret >= 0) {
		FAIL("expected failure for 17-char name, got %d", ret);
		return;
	}
	PASS();
}

static void test_buffer_too_small(void)
{
	uint8_t edid[CASTKMS_EDID_BLOCK];
	int ret;

	TEST(buffer_too_small);
	ret = castkms_fill_edid(edid, sizeof(edid), NULL,
				CASTKMS_EDID_FLAG_AUDIO);
	if (ret >= 0) {
		FAIL("expected failure for undersized buffer, got %d", ret);
		return;
	}
	PASS();
}

static void test_legacy_wrapper(void)
{
	uint8_t edid[CASTKMS_EDID_BLOCK];
	int ret;

	TEST(legacy_wrapper);
	ret = castkms_fill_named_edid(edid, NULL);
	if (ret != 0) {
		FAIL("expected 0, got %d", ret);
		return;
	}
	if (edid[126] != 0) {
		FAIL("legacy wrapper produced %u extensions", edid[126]);
		return;
	}
	if (!edid_block_checksum_valid(edid)) {
		FAIL("checksum invalid");
		return;
	}
	PASS();
}

int main(void)
{
	test_video_only_size();
	test_audio_size();
	test_video_only_no_extensions();
	test_audio_extension_count();
	test_base_block_checksum();
	test_cta_block_checksum();
	test_video_only_checksum();
	test_cta_tag_and_revision();
	test_cta_basic_audio_flag();
	test_cta_audio_data_block();
	test_cta_lpcm_sad();
	test_cta_speaker_allocation();
	test_cta_dtd_offset();
	test_named_edid();
	test_name_too_long();
	test_buffer_too_small();
	test_legacy_wrapper();

	printf("\n%d/%d tests passed\n", tests_passed, tests_run);
	return tests_passed == tests_run ? EXIT_SUCCESS : EXIT_FAILURE;
}
