/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CASTKMS_CAPTURE_H_
#define _CASTKMS_CAPTURE_H_

#include <linux/types.h>

#include "castkms_capture_output.h"

struct drm_crtc_state;
struct drm_device;
struct castkms_capture_authority;
struct castkms_capture_stream;
struct castkms_output;

int castkms_capture_output_init(struct drm_device *dev,
				struct castkms_output *output);
void castkms_capture_mode_changed(struct castkms_output *output,
				  const struct drm_crtc_state *state);

/* The caller must hold @authority through begin_output()/end(). */
struct castkms_capture_stream *
castkms_capture_stream_create(struct castkms_output *output,
			      struct castkms_capture_authority *authority,
			      u64 *mode_generation);
int castkms_capture_stream_attach(struct castkms_capture_stream *stream);
void castkms_capture_stream_destroy(struct castkms_capture_stream *stream,
				    int status);
u64 castkms_capture_stream_authority_generation(
	const struct castkms_capture_stream *stream);
int castkms_capture_stream_status(const struct castkms_capture_stream *stream);
int castkms_capture_stream_validate_mode(
	const struct castkms_capture_stream *stream, u64 mode_generation);
#endif /* _CASTKMS_CAPTURE_H_ */
