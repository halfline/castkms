/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef CASTKMS_VIRTUALSCREEN_EDID_H
#define CASTKMS_VIRTUALSCREEN_EDID_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CASTKMS_EDID_BLOCK 128
#define CASTKMS_EDID_MAX_BLOCKS 2

#define CASTKMS_EDID_FLAG_AUDIO (1 << 0)

/* Digital separate sync, +hsync +vsync (CEA-861). */
#define CASTKMS_EDID_DTD_FEATURES_CEA	0x1e

static void castkms_edid_set_checksum(uint8_t *edid, size_t size)
{
	size_t block;

	for (block = 0; block < size; block += CASTKMS_EDID_BLOCK) {
		unsigned int sum = 0;
		unsigned int i;

		edid[block + CASTKMS_EDID_BLOCK - 1] = 0;
		for (i = 0; i < CASTKMS_EDID_BLOCK; i++)
			sum += edid[block + i];
		edid[block + CASTKMS_EDID_BLOCK - 1] =
			(uint8_t)(256 - (sum & 0xff));
	}
}

static void castkms_edid_write_dtd(uint8_t *dtd, unsigned int clock_10khz,
				   unsigned int hactive, unsigned int hblank,
				   unsigned int vactive, unsigned int vblank,
				   unsigned int hfront, unsigned int hsync,
				   unsigned int vfront, unsigned int vsync)
{
	dtd[0] = clock_10khz & 0xff;
	dtd[1] = (clock_10khz >> 8) & 0xff;
	dtd[2] = hactive & 0xff;
	dtd[3] = hblank & 0xff;
	dtd[4] = ((hactive >> 4) & 0xf0) | ((hblank >> 8) & 0x0f);
	dtd[5] = vactive & 0xff;
	dtd[6] = vblank & 0xff;
	dtd[7] = ((vactive >> 4) & 0xf0) | ((vblank >> 8) & 0x0f);
	dtd[8] = hfront & 0xff;
	dtd[9] = hsync & 0xff;
	dtd[10] = ((vfront & 0x0f) << 4) | (vsync & 0x0f);
	dtd[11] = (((hfront >> 8) & 0x03) << 6) |
		  (((hsync >> 8) & 0x03) << 4) |
		  (((vfront >> 4) & 0x03) << 2) |
		  ((vsync >> 4) & 0x03);
	dtd[17] = CASTKMS_EDID_DTD_FEATURES_CEA;
}

static void castkms_edid_write_cta_audio(uint8_t *ext)
{
	/*
	 * CTA-861 extension block with basic audio support:
	 *   - LPCM Short Audio Descriptor: 2ch, 32/44.1/48 kHz, 16-bit
	 *   - Speaker Allocation: FL/FR
	 */
	memset(ext, 0, CASTKMS_EDID_BLOCK);

	/* CTA extension tag and revision. */
	ext[0] = 0x02;
	ext[1] = 0x03;

	/*
	 * Byte 3: offset to first DTD (or end of data blocks).
	 * Data blocks start at byte 4.
	 *
	 * Audio data block: tag 1, length 3 (one SAD).
	 *   Byte 0: (tag << 5) | length = (1 << 5) | 3 = 0x23
	 *   Byte 1: SAD byte 1 — format=LPCM (1), channels-1=1 → (1<<3)|1 = 0x09
	 *   Byte 2: SAD byte 2 — rates: 48kHz|44.1kHz|32kHz = 0x07
	 *   Byte 3: SAD byte 3 — 16-bit = 0x01
	 *
	 * Speaker allocation block: tag 4, length 3.
	 *   Byte 0: (tag << 5) | length = (4 << 5) | 3 = 0x83
	 *   Byte 1: FL/FR = 0x01
	 *   Byte 2: 0x00
	 *   Byte 3: 0x00
	 */

	/* Audio data block: tag=1, length=3. */
	ext[4] = 0x23;
	/* LPCM, 2 channels. */
	ext[5] = 0x09;
	/* 32 kHz, 44.1 kHz, 48 kHz. */
	ext[6] = 0x07;
	/* 16-bit samples. */
	ext[7] = 0x01;

	/* Speaker allocation block: tag=4, length=3. */
	ext[8] = 0x83;
	/* FL/FR. */
	ext[9] = 0x01;

	/* DTD offset: data blocks span bytes 4..11, so DTDs start at 12. */
	ext[2] = 12;

	/*
	 * Byte 3 feature bits: basic audio supported, no native DTDs.
	 * Bit 6 = basic audio.
	 */
	ext[3] = 0x40;

	/* Checksum is set by the caller over the whole EDID. */
}

/*
 * Generate a VirtualScreen EDID.
 *
 * When flags includes CASTKMS_EDID_FLAG_AUDIO, write a two-block EDID
 * with a CTA-861 extension containing a stereo LPCM Short Audio
 * Descriptor.  Otherwise write a single base block with no audio.
 *
 * Returns the total EDID size in bytes, or -1 on error.
 */
static int castkms_fill_edid(uint8_t *edid, size_t buf_size,
			     const char *name, unsigned int flags)
{
	size_t name_len;
	size_t total_size;
	unsigned int num_extensions;
	size_t i;

	if (!name)
		name = "VirtualScreen";
	name_len = strlen(name);
	if (name_len > 13)
		return -1;

	num_extensions = (flags & CASTKMS_EDID_FLAG_AUDIO) ? 1 : 0;
	total_size = CASTKMS_EDID_BLOCK * (1 + num_extensions);
	if (buf_size < total_size)
		return -1;

	memset(edid, 0, total_size);
	edid[0] = 0x00;
	edid[1] = 0xff;
	edid[2] = 0xff;
	edid[3] = 0xff;
	edid[4] = 0xff;
	edid[5] = 0xff;
	edid[6] = 0xff;
	edid[7] = 0x00;
	edid[8] = 0x0d;
	edid[9] = 0x6d;
	edid[10] = 0x01;
	edid[16] = 1;
	edid[17] = 34;
	edid[18] = 1;
	edid[19] = 3;
	edid[20] = 0x80;
	edid[23] = 120;
	edid[24] = 0x0a;
	edid[25] = 0xee;
	edid[26] = 0x91;
	edid[27] = 0xa3;
	edid[28] = 0x54;
	edid[29] = 0x4c;
	edid[30] = 0x99;
	edid[31] = 0x26;
	edid[32] = 0x0f;
	edid[33] = 0x50;
	edid[34] = 0x54;
	/* Established: 640x480@60, 800x600@60, 1024x768@60. */
	edid[35] = 0x21;
	edid[36] = 0x08;
	/* Standard timing 0: 1920x1080@60 16:9. */
	edid[38] = (1920 / 8) - 31;
	edid[39] = 0xc0;
	for (i = 40; i < 54; i += 2) {
		edid[i] = 0x01;
		edid[i + 1] = 0x01;
	}
	/* Preferred DTD: CEA 1920x1080@60. */
	castkms_edid_write_dtd(&edid[54], 14850,
			       1920, 280, 1080, 45, 88, 44, 4, 5);
	edid[75] = 0xfc;
	memcpy(&edid[77], name, name_len);
	if (name_len < 13) {
		edid[77 + name_len] = 0x0a;
		for (i = name_len + 1; i < 13; i++)
			edid[77 + i] = 0x20;
	}
	/* Additional DTD: CEA 3840x2160@60. */
	castkms_edid_write_dtd(&edid[90], 59400,
			       3840, 560, 2160, 90, 176, 88, 8, 10);
	edid[111] = 0x10;

	/* Extension count in base block byte 126. */
	edid[126] = (uint8_t)num_extensions;

	if (flags & CASTKMS_EDID_FLAG_AUDIO)
		castkms_edid_write_cta_audio(&edid[CASTKMS_EDID_BLOCK]);

	castkms_edid_set_checksum(edid, total_size);
	return (int)total_size;
}

/*
 * Legacy wrapper: fill a single base block without audio.
 * Retained for callers that allocate exactly one block.
 */
__attribute__((unused))
static int castkms_fill_named_edid(uint8_t edid[CASTKMS_EDID_BLOCK],
				   const char *name)
{
	int ret;

	ret = castkms_fill_edid(edid, CASTKMS_EDID_BLOCK, name, 0);
	if (ret < 0)
		return ret;
	return 0;
}

#endif /* CASTKMS_VIRTUALSCREEN_EDID_H */
