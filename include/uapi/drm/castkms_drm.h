/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */

#ifndef _UAPI_CASTKMS_DRM_H_
#define _UAPI_CASTKMS_DRM_H_

#include <drm/drm.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define DRM_CASTKMS_CAPTURE_UAPI_MAJOR	0
#define DRM_CASTKMS_CAPTURE_UAPI_MINOR	5

/**
 * DRM_CASTKMS_CAPTURE_CAP_SYNCOBJ_TIMELINE:
 *
 * Capture buffers may use DRM timeline syncobjs for explicit synchronization.
 * The driver waits asynchronously for a reuse point and publishes a capture
 * fence at the corresponding ready point.
 */
#define DRM_CASTKMS_CAPTURE_CAP_SYNCOBJ_TIMELINE	(1ULL << 0)

/**
 * DRM_CASTKMS_CAPTURE_CAP_IMPLICIT_SYNC:
 *
 * Capture buffers may be registered without explicit timeline syncobjs.
 * Queueing waits asynchronously for prior users through the GEM reservation
 * object and publishes a producer fence before returning.
 */
#define DRM_CASTKMS_CAPTURE_CAP_IMPLICIT_SYNC		(1ULL << 1)

/**
 * DRM_CASTKMS_CAPTURE_CAP_DMA_BUF_IMPORT:
 *
 * Capture destinations may be framebuffers backed by DMA-BUFs imported from
 * another DRM device. When this bit is clear, destinations must use GEM
 * objects created by castkms; clients must not probe support by registering an
 * imported framebuffer and interpreting the returned errno.
 */
#define DRM_CASTKMS_CAPTURE_CAP_DMA_BUF_IMPORT		(1ULL << 2)

/**
 * struct drm_castkms_capture_format - capture buffer format
 * @format: DRM_FORMAT_* fourcc value
 * @flags: format-specific flags; must be zero
 * @modifier: DRM_FORMAT_MOD_* layout modifier
 */
struct drm_castkms_capture_format {
	__u32 format;
	__u32 flags;
	__u64 modifier;
};

/**
 * struct drm_castkms_capture_query_caps - query capture capabilities
 * @uapi_major: capture UAPI major version returned by the driver
 * @uapi_minor: capture UAPI minor version returned by the driver
 * @crtc_id: DRM object ID of the CRTC to query
 * @format_count: input array capacity and output number of supported formats
 * @flags: bitmask of DRM_CASTKMS_CAPTURE_CAP_* values
 * @formats_ptr: userspace pointer to an array of supported formats, or zero
 * @max_registered_buffers: maximum buffers accepted by a capture stream
 * @reserved: must be zero
 *
 * Call the ioctl with @format_count and @formats_ptr set to zero to discover
 * the required array length. Allocate that many entries, set @format_count to
 * the array capacity and @formats_ptr to the array address, then call it again
 * to retrieve the entries.
 */
struct drm_castkms_capture_query_caps {
	__u32 uapi_major;
	__u32 uapi_minor;
	__u32 crtc_id;
	__u32 format_count;
	__u64 flags;
	__u64 formats_ptr;
	__u32 max_registered_buffers;
	__u32 reserved;
};

#define DRM_CASTKMS_CAPTURE_QUERY_CAPS	0x00

#define DRM_IOCTL_CASTKMS_CAPTURE_QUERY_CAPS \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_CASTKMS_CAPTURE_QUERY_CAPS, \
		 struct drm_castkms_capture_query_caps)

#if defined(__cplusplus)
}
#endif

#endif /* _UAPI_CASTKMS_DRM_H_ */
