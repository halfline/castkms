// SPDX-License-Identifier: GPL-2.0+

#include <kunit/test.h>

#include <drm/drm_fixed.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_mode.h>
#include <drm/drm_rect.h>

#include "../castkms_composer.h"
#include "../castkms_formats.h"
#include "../castkms_output_buffer.h"
#include "../castkms_snapshot.h"

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

struct snapshot_test_plane {
	struct castkms_snapshot_plane sp;
	struct drm_gem_object obj;
	struct drm_framebuffer fb;
};

static void init_test_plane(struct snapshot_test_plane *tp, u32 format,
			    void *pixels, u32 width, u32 height)
{
	memset(tp, 0, sizeof(*tp));

	tp->fb.format = drm_format_info(format);
	tp->fb.width = width;
	tp->fb.height = height;
	tp->fb.pitches[0] = width * tp->fb.format->cpp[0];
	tp->fb.obj[0] = &tp->obj;

	iosys_map_set_vaddr(&tp->sp.map[0], pixels);

	tp->sp.frame_info.fb = &tp->fb;
	tp->sp.frame_info.map = tp->sp.map;
	tp->sp.frame_info.src = (struct drm_rect){
		.x1 = 0, .y1 = 0,
		.x2 = width << 16, .y2 = height << 16
	};
	tp->sp.frame_info.dst = (struct drm_rect){
		.x1 = 0, .y1 = 0,
		.x2 = width, .y2 = height
	};
	tp->sp.frame_info.rotation = DRM_MODE_ROTATE_0;

	tp->sp.plane.frame_info = &tp->sp.frame_info;
	tp->sp.plane.pixel_read_line =
		castkms_get_pixel_read_line_function(format);
}

struct snapshot_test_output {
	struct castkms_output_buffer output;
	struct drm_gem_object obj;
	struct drm_framebuffer fb;
};

struct snapshot_test_fence {
	struct dma_fence base;
	spinlock_t lock;
};

static const char *snapshot_test_fence_name(struct dma_fence *fence)
{
	(void)fence;
	return "castkms-test";
}

static void snapshot_test_fence_release(struct dma_fence *fence)
{
	kfree(container_of(fence, struct snapshot_test_fence, base));
}

static const struct dma_fence_ops snapshot_test_fence_ops = {
	.get_driver_name = snapshot_test_fence_name,
	.get_timeline_name = snapshot_test_fence_name,
	.release = snapshot_test_fence_release,
};

static struct dma_fence *snapshot_test_unsignaled_fence(void)
{
	struct snapshot_test_fence *fence;

	fence = kzalloc_obj(*fence);
	if (!fence)
		return NULL;
	spin_lock_init(&fence->lock);
	dma_fence_init64(&fence->base, &snapshot_test_fence_ops, &fence->lock,
			 dma_fence_context_alloc(1), 1);
	return &fence->base;
}

static void init_test_output(struct snapshot_test_output *to, u32 format,
			     void *pixels, u32 width, u32 height)
{
	memset(to, 0, sizeof(*to));

	to->fb.format = drm_format_info(format);
	to->fb.width = width;
	to->fb.height = height;
	to->fb.pitches[0] = width * to->fb.format->cpp[0];
	to->fb.obj[0] = &to->obj;

	iosys_map_set_vaddr(&to->output.map[0], pixels);
	to->output.fb = &to->fb;
	to->output.write_pixel =
		castkms_get_pixel_write_function(format);
}

static void castkms_snapshot_test_compose_single_plane(struct kunit *test)
{
	u8 src_pixels[] = {
		0xff, 0x00, 0x00, 0xff, /* XRGB: blue */
		0x00, 0xff, 0x00, 0xff, /* XRGB: green */
	};
	u8 dst_pixels[8];
	struct snapshot_test_plane tp;
	struct snapshot_test_output to;
	struct castkms_frame_plane *planes[] = { &tp.sp.plane };
	struct castkms_frame_stage frame = {
		.planes = planes,
		.num_planes = ARRAY_SIZE(planes),
		.width = 2,
		.height = 1,
	};
	int ret;

	init_test_plane(&tp, DRM_FORMAT_XRGB8888, src_pixels, 2, 1);
	init_test_output(&to, DRM_FORMAT_XRGB8888, dst_pixels, 2, 1);

	memset(dst_pixels, 0, sizeof(dst_pixels));
	ret = castkms_compose_frame(&frame, &to.output);

	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, dst_pixels[0], (u8)0xff);
	KUNIT_EXPECT_EQ(test, dst_pixels[1], (u8)0x00);
	KUNIT_EXPECT_EQ(test, dst_pixels[2], (u8)0x00);
	KUNIT_EXPECT_EQ(test, dst_pixels[4], (u8)0x00);
	KUNIT_EXPECT_EQ(test, dst_pixels[5], (u8)0xff);
	KUNIT_EXPECT_EQ(test, dst_pixels[6], (u8)0x00);
}

static void castkms_snapshot_test_compose_background(struct kunit *test)
{
	u8 dst_pixels[4];
	struct snapshot_test_output to;
	struct castkms_frame_stage frame = {
		.width = 1,
		.height = 1,
		.background_color =
			DRM_ARGB64_PREP(0xffff, 0xffff, 0x0000, 0x0000),
	};
	int ret;

	init_test_output(&to, DRM_FORMAT_XRGB8888, dst_pixels, 1, 1);

	memset(dst_pixels, 0, sizeof(dst_pixels));
	ret = castkms_compose_frame(&frame, &to.output);

	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, dst_pixels[0], (u8)0x00);
	KUNIT_EXPECT_EQ(test, dst_pixels[1], (u8)0x00);
	KUNIT_EXPECT_EQ(test, dst_pixels[2], (u8)0xff);
}

static void castkms_snapshot_test_compose_no_destination(struct kunit *test)
{
	struct castkms_frame_stage frame = {
		.width = 1,
		.height = 1,
	};
	int ret;

	ret = castkms_compose_frame(&frame, NULL);

	KUNIT_ASSERT_EQ(test, ret, 0);
}

static void castkms_snapshot_test_compose_with_gamma(struct kunit *test)
{
	u8 src_pixels[] = { 0x00, 0x80, 0x00, 0xff };
	u8 dst_pixels[4];
	struct snapshot_test_plane tp;
	struct snapshot_test_output to;
	struct castkms_frame_plane *planes[] = { &tp.sp.plane };
	struct castkms_frame_stage frame = {
		.planes = planes,
		.num_planes = ARRAY_SIZE(planes),
		.width = 1,
		.height = 1,
	};
	struct drm_color_lut *lut;
	int ret;
	size_t i;

	lut = kunit_kzalloc(test, CASTKMS_LUT_SIZE * sizeof(*lut), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, lut);

	for (i = 0; i < CASTKMS_LUT_SIZE; i++) {
		lut[i].red = 0xffff;
		lut[i].green = 0xffff;
		lut[i].blue = 0xffff;
	}

	init_test_plane(&tp, DRM_FORMAT_XRGB8888, src_pixels, 1, 1);
	init_test_output(&to, DRM_FORMAT_XRGB8888, dst_pixels, 1, 1);

	frame.gamma_lut.base = lut;
	frame.gamma_lut.lut_length = CASTKMS_LUT_SIZE;
	frame.gamma_lut.channel_value2index_ratio =
		drm_fixp_div(drm_int2fixp(CASTKMS_LUT_SIZE - 1),
			     drm_int2fixp(0xffff));

	memset(dst_pixels, 0, sizeof(dst_pixels));
	ret = castkms_compose_frame(&frame, &to.output);

	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, dst_pixels[0], (u8)0xff);
	KUNIT_EXPECT_EQ(test, dst_pixels[1], (u8)0xff);
	KUNIT_EXPECT_EQ(test, dst_pixels[2], (u8)0xff);
}

static void castkms_snapshot_test_plane_pixel_read(struct kunit *test)
{
	u8 src_pixels[] = { 0xaa, 0xbb, 0xcc, 0xff };
	struct snapshot_test_plane tp;
	struct pixel_argb_u16 pixel;

	init_test_plane(&tp, DRM_FORMAT_XRGB8888, src_pixels, 1, 1);
	KUNIT_ASSERT_NOT_NULL(test, tp.sp.plane.pixel_read_line);

	tp.sp.plane.pixel_read_line(&tp.sp.plane, 0, 0,
				    READ_LEFT_TO_RIGHT, 1, &pixel);

	KUNIT_EXPECT_EQ(test, pixel.b, (u16)0xaaaa);
	KUNIT_EXPECT_EQ(test, pixel.g, (u16)0xbbbb);
	KUNIT_EXPECT_EQ(test, pixel.r, (u16)0xcccc);
}

static void castkms_snapshot_test_rejects_iomem_source(struct kunit *test)
{
	u8 src_pixels[] = { 0xff, 0x00, 0x00, 0xff };
	struct snapshot_test_plane tp;

	init_test_plane(&tp, DRM_FORMAT_XRGB8888, src_pixels, 1, 1);
	iosys_map_set_vaddr_iomem(&tp.sp.map[0], (void __iomem *)src_pixels);

	KUNIT_EXPECT_FALSE(test,
			   castkms_framebuffer_maps_are_accessible(&tp.fb,
								   tp.sp.map));
}

static void castkms_snapshot_test_rejects_null_map(struct kunit *test)
{
	struct snapshot_test_plane tp;

	init_test_plane(&tp, DRM_FORMAT_XRGB8888, NULL, 1, 1);
	memset(&tp.sp.map[0], 0, sizeof(tp.sp.map[0]));

	KUNIT_EXPECT_FALSE(test,
			   castkms_framebuffer_maps_are_accessible(&tp.fb,
								   tp.sp.map));
}

static void castkms_snapshot_test_source_wait_is_bounded(struct kunit *test)
{
	struct dma_fence *fence = snapshot_test_unsignaled_fence();
	struct dma_fence *fences[] = { fence };

	KUNIT_ASSERT_NOT_NULL(test, fence);
	KUNIT_EXPECT_EQ(test, castkms_snapshot_wait_fences(fences, 1, 1),
			-ETIMEDOUT);
	dma_fence_put(fence);
}

static struct kunit_case castkms_snapshot_test_cases[] = {
	KUNIT_CASE(castkms_snapshot_test_compose_single_plane),
	KUNIT_CASE(castkms_snapshot_test_compose_background),
	KUNIT_CASE(castkms_snapshot_test_compose_no_destination),
	KUNIT_CASE(castkms_snapshot_test_compose_with_gamma),
	KUNIT_CASE(castkms_snapshot_test_plane_pixel_read),
	KUNIT_CASE(castkms_snapshot_test_rejects_iomem_source),
	KUNIT_CASE(castkms_snapshot_test_rejects_null_map),
	KUNIT_CASE(castkms_snapshot_test_source_wait_is_bounded),
	{}
};

static struct kunit_suite castkms_snapshot_test_suite = {
	.name = "castkms-snapshot",
	.test_cases = castkms_snapshot_test_cases,
};
kunit_test_suite(castkms_snapshot_test_suite);

MODULE_LICENSE("GPL");
