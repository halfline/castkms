/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef CASTKMS_VIRTUALSCREEN_EDID_H
#define CASTKMS_VIRTUALSCREEN_EDID_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CASTKMS_EDID_BLOCK 128
#define CASTKMS_EDID_MAX_BLOCKS 2

#define CASTKMS_EDID_FLAG_AUDIO (1 << 0)
#define CASTKMS_EDID_FLAG_CEC   (1 << 1)

/* Digital separate sync, +hsync +vsync (CEA-861). */
#define CASTKMS_EDID_DTD_FEATURES_CEA	0x1e

/*
 * Validate a CEC physical address in dotted form (A.B.C.D).
 * Each nibble must be 0-15. A nonzero nibble must not follow a zero nibble.
 * 0.0.0.0 is reserved for the root TV and not valid as a source address.
 */
__attribute__((unused))
static int castkms_edid_validate_phys_addr(unsigned int a, unsigned int b,
					   unsigned int c, unsigned int d)
{
	if (a > 15 || b > 15 || c > 15 || d > 15)
		return -1;
	if (a == 0 && b == 0 && c == 0 && d == 0)
		return -1;
	if (a == 0 && (b || c || d))
		return -1;
	if (b == 0 && (c || d))
		return -1;
	if (c == 0 && d)
		return -1;
	return 0;
}

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

/*
 * Write a CTA-861 extension block.
 *
 * @ext: pointer to the 128-byte extension block
 * @flags: CASTKMS_EDID_FLAG_AUDIO and/or CASTKMS_EDID_FLAG_CEC
 * @phys_addr_ab: high byte of CEC physical address (A<<4 | B)
 * @phys_addr_cd: low byte of CEC physical address (C<<4 | D)
 */
static void castkms_edid_write_cta(uint8_t *ext, unsigned int flags,
				   uint8_t phys_addr_ab,
				   uint8_t phys_addr_cd)
{
	unsigned int pos = 4;
	uint8_t features = 0;

	memset(ext, 0, CASTKMS_EDID_BLOCK);

	/* CTA extension tag and revision. */
	ext[0] = 0x02;
	ext[1] = 0x03;

	if (flags & CASTKMS_EDID_FLAG_AUDIO) {
		/* Audio data block: tag=1, length=3 (one SAD). */
		ext[pos++] = 0x23;
		/* LPCM, 2 channels. */
		ext[pos++] = 0x09;
		/* 32 kHz, 44.1 kHz, 48 kHz. */
		ext[pos++] = 0x07;
		/* 16-bit samples. */
		ext[pos++] = 0x01;

		/* Speaker allocation block: tag=4, length=3. */
		ext[pos++] = 0x83;
		/* FL/FR. */
		ext[pos++] = 0x01;
		/* Padding bytes. */
		pos += 2;

		features |= 0x40; /* basic audio */
	}

	if (flags & CASTKMS_EDID_FLAG_CEC) {
		/*
		 * HDMI Vendor-Specific Data Block: tag=3, length=5.
		 * OUI 0x000c03 in little-endian: 0x03 0x0c 0x00.
		 * Followed by the 2-byte source physical address.
		 */
		ext[pos++] = (3 << 5) | 5;
		ext[pos++] = 0x03;
		ext[pos++] = 0x0c;
		ext[pos++] = 0x00;
		ext[pos++] = phys_addr_ab;
		ext[pos++] = phys_addr_cd;
	}

	/* DTD offset: data blocks end at pos. */
	ext[2] = (uint8_t)pos;

	/* Feature bits. */
	ext[3] = features;

	/* Checksum is set by the caller over the whole EDID. */
}

/*
 * Generate a VirtualScreen EDID.
 *
 * When flags includes CASTKMS_EDID_FLAG_AUDIO and/or CASTKMS_EDID_FLAG_CEC,
 * write a two-block EDID with a CTA-861 extension. CASTKMS_EDID_FLAG_CEC
 * requires CASTKMS_EDID_FLAG_AUDIO or stands alone — either way a CTA
 * extension is produced.
 *
 * @phys_addr_ab and @phys_addr_cd encode the CEC physical address:
 *   A.B.C.D → ab = (A<<4)|B, cd = (C<<4)|D.
 * They are ignored when CASTKMS_EDID_FLAG_CEC is not set.
 *
 * Returns the total EDID size in bytes, or -1 on error.
 */
__attribute__((unused))
static int castkms_fill_edid_full(uint8_t *edid, size_t buf_size,
				  const char *name, unsigned int flags,
				  uint8_t phys_addr_ab, uint8_t phys_addr_cd)
{
	size_t name_len;
	size_t total_size;
	unsigned int num_extensions;
	unsigned int cta_flags;
	size_t i;

	if (!name)
		name = "VirtualScreen";
	name_len = strlen(name);
	if (name_len > 13)
		return -1;

	cta_flags = flags & (CASTKMS_EDID_FLAG_AUDIO | CASTKMS_EDID_FLAG_CEC);
	num_extensions = cta_flags ? 1 : 0;
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

	if (cta_flags)
		castkms_edid_write_cta(&edid[CASTKMS_EDID_BLOCK], cta_flags,
				       phys_addr_ab, phys_addr_cd);

	castkms_edid_set_checksum(edid, total_size);
	return (int)total_size;
}

/*
 * Convenience wrapper: generate an EDID without CEC physical address.
 * Audio-only and video-only callers use this.
 */
static int castkms_fill_edid(uint8_t *edid, size_t buf_size,
			     const char *name, unsigned int flags)
{
	return castkms_fill_edid_full(edid, buf_size, name, flags, 0, 0);
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
