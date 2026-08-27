// SPDX-License-Identifier: GPL-2.0-only

#include <linux/iosys-map.h>
#include <linux/slab.h>

#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_framebuffer_helper.h>

#include "castkms_capture.h"
#include "castkms_capture_authority.h"
#include "castkms_capture_internal.h"
#include "castkms_frame.h"
#include "castkms_output.h"

static int castkms_capture_cursor_copy_bitmap(
	struct castkms_capture_buffer *buffer,
	const struct castkms_cursor_snapshot *cursor)
{
	struct iosys_map map[DRM_FORMAT_MAX_PLANES] = {};
	u32 source_stride = cursor->fb->pitches[0];
	u32 stride;
	u32 size;
	u32 y;
	void *bitmap;
	int ret;

	if (cursor->fb->format->format != DRM_FORMAT_ARGB8888 ||
	    !cursor->width || !cursor->height ||
	    cursor->width > cursor->fb->dev->mode_config.cursor_width ||
	    cursor->height > cursor->fb->dev->mode_config.cursor_height)
		return -EINVAL;
	stride = cursor->width * 4U;
	if (source_stride < stride)
		return -EINVAL;
	size = stride * cursor->height;

	ret = drm_gem_fb_vmap(cursor->fb, map, NULL);
	if (ret)
		return ret;

	bitmap = krealloc(buffer->cursor_bitmap, size, GFP_KERNEL);
	if (!bitmap) {
		drm_gem_fb_vunmap(cursor->fb, map);
		return -ENOMEM;
	}

	/* Export a bounded, tightly packed ARGB8888 image without FB padding. */
	for (y = 0; y < cursor->height; y++)
		iosys_map_memcpy_from((u8 *)bitmap + (size_t)y * stride, &map[0],
				      (size_t)y * source_stride, stride);
	drm_gem_fb_vunmap(cursor->fb, map);

	buffer->cursor_bitmap = bitmap;
	buffer->cursor_bitmap_size = size;
	buffer->cursor_bitmap_stride = stride;
	buffer->cursor_bitmap_serial = cursor->serial;

	return 0;
}

static void
castkms_capture_cursor_clear_bitmap(struct castkms_capture_buffer *buffer)
{
	kfree(buffer->cursor_bitmap);
	buffer->cursor_bitmap = NULL;
	buffer->cursor_bitmap_size = 0;
	buffer->cursor_bitmap_stride = 0;
	buffer->cursor_bitmap_serial = 0;
}

int castkms_capture_buffer_set_cursor(struct castkms_capture_buffer *buffer,
				      const struct castkms_cursor_snapshot *cursor)
{
	struct castkms_capture_stream *stream = buffer->stream;
	int ret;

	if (!castkms_capture_authority_has_rights(
		    stream->authority, CASTKMS_CAPTURE_AUTHORITY_READ_CURSOR))
		cursor = NULL;

	/* Serial zero is the initial, never-visible cursor state, not a hide. */
	if (!cursor || (!cursor->visible && !cursor->serial)) {
		stream->cursor_serial = 0;
		stream->cursor_serial_valid = false;
		castkms_capture_cursor_clear_bitmap(buffer);
		buffer->cursor_serial = 0;
		buffer->cursor_visible = false;
		buffer->cursor_image_changed = false;
		buffer->cursor_x = 0;
		buffer->cursor_y = 0;
		buffer->cursor_hotspot_x = 0;
		buffer->cursor_hotspot_y = 0;
		buffer->cursor_width = 0;
		buffer->cursor_height = 0;
		return 0;
	}

	if (!cursor->visible) {
		buffer->cursor_visible = false;
		buffer->cursor_image_changed =
			!stream->cursor_serial_valid ||
			cursor->serial != stream->cursor_serial;
		stream->cursor_serial = cursor->serial;
		stream->cursor_serial_valid = true;
		castkms_capture_cursor_clear_bitmap(buffer);
		buffer->cursor_serial = cursor->serial;
		buffer->cursor_x = 0;
		buffer->cursor_y = 0;
		buffer->cursor_hotspot_x = 0;
		buffer->cursor_hotspot_y = 0;
		buffer->cursor_width = 0;
		buffer->cursor_height = 0;
		return 0;
	}

	buffer->cursor_visible = true;
	buffer->cursor_image_changed = false;
	if (!stream->cursor_serial_valid ||
	    cursor->serial != stream->cursor_serial) {
		if (!cursor->fb)
			return -EINVAL;
		ret = castkms_capture_cursor_copy_bitmap(buffer, cursor);
		if (ret) {
			castkms_capture_cursor_clear_bitmap(buffer);
			buffer->cursor_serial = 0;
			buffer->cursor_visible = false;
			buffer->cursor_image_changed = false;
			return ret;
		}
		stream->cursor_serial = cursor->serial;
		stream->cursor_serial_valid = true;
		buffer->cursor_image_changed = true;
	}
	buffer->cursor_serial = cursor->serial;
	buffer->cursor_x = cursor->x;
	buffer->cursor_y = cursor->y;
	buffer->cursor_hotspot_x = cursor->hotspot_x;
	buffer->cursor_hotspot_y = cursor->hotspot_y;
	buffer->cursor_width = cursor->width;
	buffer->cursor_height = cursor->height;

	return 0;
}

int castkms_capture_buffer_get_cursor_data(
	struct castkms_capture_buffer *buffer,
	struct castkms_capture_cursor_data *cursor)
{
	struct castkms_capture_output *capture = &buffer->stream->output->capture;
	unsigned long flags;

	if (!cursor)
		return -EINVAL;

	spin_lock_irqsave(&capture->state_lock, flags);
	if (buffer->state != CASTKMS_CAPTURE_BUFFER_IDLE) {
		spin_unlock_irqrestore(&capture->state_lock, flags);
		return -EBUSY;
	}

	*cursor = (struct castkms_capture_cursor_data) {};
	if (buffer->cursor_bitmap &&
	    buffer->cursor_bitmap_serial == buffer->cursor_serial) {
		*cursor = (struct castkms_capture_cursor_data) {
			.bitmap = buffer->cursor_bitmap,
			.size = buffer->cursor_bitmap_size,
			.stride = buffer->cursor_bitmap_stride,
			.width = buffer->cursor_width,
			.height = buffer->cursor_height,
		};
	}
	spin_unlock_irqrestore(&capture->state_lock, flags);

	return 0;
}
