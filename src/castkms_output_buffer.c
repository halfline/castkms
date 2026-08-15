// SPDX-License-Identifier: GPL-2.0+

#include <linux/dma-direction.h>
#include <linux/errno.h>
#include <linux/minmax.h>

#include <drm/drm_gem_framebuffer_helper.h>

#include <kunit/visibility.h>

#include "castkms_formats.h"
#include "castkms_output_buffer.h"

int castkms_output_buffer_init(struct castkms_output_buffer *buffer,
			       struct drm_framebuffer *fb)
{
	pixel_write_t write_pixel;
	int ret;

	write_pixel = castkms_get_pixel_write_function(fb->format->format);
	if (!write_pixel)
		return -EINVAL;

	drm_framebuffer_get(fb);
	ret = drm_gem_fb_vmap(fb, buffer->map, NULL);
	if (ret)
		goto err_put_framebuffer;

	if (!castkms_framebuffer_maps_are_accessible(fb, buffer->map)) {
		ret = -EOPNOTSUPP;
		goto err_vunmap;
	}

	buffer->fb = fb;
	buffer->write_pixel = write_pixel;

	return 0;

err_vunmap:
	drm_gem_fb_vunmap(fb, buffer->map);
err_put_framebuffer:
	drm_framebuffer_put(fb);
	return ret;
}

void castkms_output_buffer_fini(struct castkms_output_buffer *buffer)
{
	struct drm_framebuffer *fb = buffer->fb;

	if (!fb)
		return;

	drm_gem_fb_vunmap(fb, buffer->map);
	drm_framebuffer_put(fb);

	buffer->fb = NULL;
	buffer->write_pixel = NULL;
}

bool castkms_output_buffer_is_valid(const struct castkms_output_buffer *buffer)
{
	if (!buffer || !buffer->fb || !buffer->write_pixel)
		return false;

	return castkms_framebuffer_maps_are_accessible(buffer->fb, buffer->map);
}
EXPORT_SYMBOL_IF_KUNIT(castkms_output_buffer_is_valid);

int castkms_output_buffer_begin_cpu_access(const struct castkms_output_buffer *buffer)
{
	return drm_gem_fb_begin_cpu_access(buffer->fb, DMA_TO_DEVICE);
}

void castkms_output_buffer_end_cpu_access(const struct castkms_output_buffer *buffer)
{
	drm_gem_fb_end_cpu_access(buffer->fb, DMA_TO_DEVICE);
}

void
castkms_output_buffer_write_row(const struct castkms_output_buffer *buffer,
				const struct line_buffer *src_buffer, int y)
{
	const struct pixel_argb_u16 *in_pixels = src_buffer->pixels;
	int x_limit, rem_x, rem_y;
	size_t offset;
	u8 *dst_pixels;

	offset = castkms_packed_pixels_offset(buffer->fb, 0, y, 0,
					      &rem_x, &rem_y);
	dst_pixels = (u8 *)buffer->map[0].vaddr + offset;
	x_limit = min_t(size_t, buffer->fb->width, src_buffer->n_pixels);

	for (size_t x = 0; x < x_limit; x++) {
		buffer->write_pixel(dst_pixels, &in_pixels[x]);
		dst_pixels += buffer->fb->format->cpp[0];
	}
}
EXPORT_SYMBOL_IF_KUNIT(castkms_output_buffer_write_row);
