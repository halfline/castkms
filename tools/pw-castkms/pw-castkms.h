// SPDX-License-Identifier: GPL-2.0-only

#ifndef PW_CASTKMS_H
#define PW_CASTKMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drm/castkms_drm.h>

#include <pipewire/pipewire.h>

struct captured_frame {
	uint64_t sequence;
	int64_t timestamp_ns;
	uint32_t flags;
	uint32_t dropped_frames;
};

struct pw_castkms {
	/* The opened primary node owns the DRM object namespace. */
	int drm_fd;
	char card_label[256];

	/* Selected connector and the compositor's active mode. */
	uint32_t connector_id;
	uint32_t crtc_id;
	char connector_name[64];
	uint32_t width;
	uint32_t height;
	uint32_t refresh;
	bool attached_here;

	/* Process lifetime and diagnostics. */
	bool failed;
	bool shutting_down;
	int exit_status;
	uint64_t frames_produced;

};

void pw_castkms_fail(struct pw_castkms *bridge, const char *operation,
		     int status);

#endif /* PW_CASTKMS_H */
