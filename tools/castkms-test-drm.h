// SPDX-License-Identifier: GPL-2.0-only

#ifndef CASTKMS_TEST_DRM_H
#define CASTKMS_TEST_DRM_H

#include <stdbool.h>
#include <stdint.h>

struct drm_castkms_capture_start;

struct castkms_test_framebuffer {
	uint32_t handle;
	uint32_t fb_id;
	uint32_t pitch;
	uint64_t size;
	void *map;
};

int castkms_test_check_driver_name(int fd);

int castkms_test_framebuffer_create(int fd, uint32_t width, uint32_t height,
				    bool map,
				    struct castkms_test_framebuffer *buffer);
void castkms_test_framebuffer_unmap(struct castkms_test_framebuffer *buffer);
void castkms_test_framebuffer_destroy(int fd,
				      struct castkms_test_framebuffer *buffer);

int castkms_test_capture_start(int fd, uint32_t crtc_id, uint32_t flags,
			       struct drm_castkms_capture_start *start);
#endif /* CASTKMS_TEST_DRM_H */
