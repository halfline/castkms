/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */

#ifndef _UAPI_CASTKMS_DRM_H_
#define _UAPI_CASTKMS_DRM_H_

#include <drm/drm.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define DRM_CASTKMS_CAPTURE_UAPI_MAJOR	0
#define DRM_CASTKMS_CAPTURE_UAPI_MINOR	8

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

/**
 * DRM_CASTKMS_CAPTURE_START_EXCLUSIVE:
 *
 * Request exclusive capture ownership of the selected CRTC.
 */
#define DRM_CASTKMS_CAPTURE_START_EXCLUSIVE	(1U << 0)

/**
 * DRM_CASTKMS_CAPTURE_START_EXCLUDE_CURSOR:
 *
 * Exclude the cursor plane from captured frame composition.  The cursor
 * position and image metadata are still reported in the capture event
 * cursor fields.  Consumers render the cursor client-side using the
 * reported metadata.
 */
#define DRM_CASTKMS_CAPTURE_START_EXCLUDE_CURSOR (1U << 1)

/**
 * struct drm_castkms_capture_start - start an exclusive capture stream
 * @crtc_id: DRM object ID of the CRTC to observe
 * @flags: DRM_CASTKMS_CAPTURE_START_EXCLUSIVE, optionally combined with
 *         DRM_CASTKMS_CAPTURE_START_EXCLUDE_CURSOR
 * @stream_id: file-local stream identifier returned by the driver
 * @reserved: must be zero
 * @mode_generation: current CRTC mode generation returned by the driver
 *
 * Starting capture does not activate or otherwise change the selected CRTC.
 * Another DRM file cannot start a stream for that CRTC until the owner stops
 * its stream or closes the file.
 */
struct drm_castkms_capture_start {
	__u32 crtc_id;
	__u32 flags;
	__u32 stream_id;
	__u32 reserved;
	__u64 mode_generation;
};

/**
 * struct drm_castkms_capture_stop - stop an owned capture stream
 * @stream_id: file-local stream identifier returned by start
 * @flags: must be zero
 * @reserved: must be zero
 */
struct drm_castkms_capture_stop {
	__u32 stream_id;
	__u32 flags;
	__u64 reserved;
};

/**
 * DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC:
 *
 * Register without explicit timeline syncobjs. The initial implementation
 * accepts GEM objects created on the castkms device and exported to consumers;
 * importing a destination from another device is not yet supported.
 */
#define DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC	(1U << 0)

/**
 * DRM_CASTKMS_CAPTURE_BUFFER_EXPLICIT_SYNC:
 *
 * Use the supplied ready and reuse timeline syncobjs. Each explicit buffer
 * requires a dedicated pair of distinct syncobjs that is not shared with
 * another buffer. The ready timeline must be empty at registration and is then
 * owned by the driver; userspace must not signal or otherwise modify it.
 */
#define DRM_CASTKMS_CAPTURE_BUFFER_EXPLICIT_SYNC	(1U << 1)
/**
 * struct drm_castkms_capture_register_buffer - register a capture destination
 * @stream_id: file-local capture stream identifier
 * @fb_id: framebuffer object ID visible to this DRM file
 * @ready_syncobj_handle: driver-produced timeline, or zero for implicit sync
 * @reuse_syncobj_handle: consumer-produced timeline, or zero for implicit sync
 * @flags: exactly one DRM_CASTKMS_CAPTURE_BUFFER_* synchronization mode
 * @buffer_id: stream-local buffer identifier returned by the driver
 * @mode_generation: generation returned when the stream was started
 *
 * The framebuffer must match the active CRTC mode and an advertised format.
 * Registration retains and maps it until it is unregistered, its stream is
 * stopped, or the DRM file is closed. Registration alone never writes it.
 */
struct drm_castkms_capture_register_buffer {
	__u32 stream_id;
	__u32 fb_id;
	__u32 ready_syncobj_handle;
	__u32 reuse_syncobj_handle;
	__u32 flags;
	__u32 buffer_id;
	__u64 mode_generation;
};

/**
 * struct drm_castkms_capture_unregister_buffer - release a capture buffer
 * @stream_id: file-local capture stream identifier
 * @buffer_id: stream-local capture buffer identifier
 * @flags: must be zero
 * @reserved: must be zero
 */
struct drm_castkms_capture_unregister_buffer {
	__u32 stream_id;
	__u32 buffer_id;
	__u32 flags;
	__u32 reserved;
};

/**
 * DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC:
 *
 * Queue a buffer registered for implicit synchronization. Existing readers
 * and writers delay vblank eligibility without blocking the vblank callback.
 */
#define DRM_CASTKMS_CAPTURE_QUEUE_IMPLICIT_SYNC	(1U << 0)

/**
 * DRM_CASTKMS_CAPTURE_QUEUE_EXPLICIT_SYNC:
 *
 * Queue a buffer registered with timeline syncobjs. The buffer becomes
 * eligible after its reuse point signals, and its ready point signals when
 * capture completes or fails.
 */
#define DRM_CASTKMS_CAPTURE_QUEUE_EXPLICIT_SYNC	(1U << 1)

/**
 * struct drm_castkms_capture_queue_buffer - queue one capture destination
 * @stream_id: file-local capture stream identifier
 * @buffer_id: stream-local registered buffer identifier
 * @flags: exactly one DRM_CASTKMS_CAPTURE_QUEUE_* synchronization mode
 * @reserved: must be zero
 * @user_data: opaque value returned in the completion event
 * @mode_generation: generation returned when the stream was started
 * @ready_point: new driver-produced timeline point, or zero for implicit sync
 * @reuse_point: consumer-produced dependency, or zero on explicit first use
 *
 * Version 0.5 accepts one queued buffer while a previous capture may remain in
 * flight on the same CRTC. A queued buffer is captured at a future vblank.
 * Implicit mode attaches a write fence to the buffer's GEM reservation object
 * before returning. Explicit mode publishes the same kind of fence at
 * @ready_point on the registered ready timeline. @ready_point must be nonzero
 * and increase on every queue. @reuse_point may be zero only on first use and
 * must then be nonzero and increase on every reuse. The referenced reuse point
 * must already exist, but need not be signaled.
 *
 * Dependencies are honored asynchronously, so an unavailable buffer is
 * skipped rather than waited upon from the vblank callback. The completion
 * event reports that the capture operation finished, but success and error
 * events are metadata only and never transfer buffer ownership.
 * Synchronization remains authoritative: wait for the explicit ready point
 * or the implicit reservation fences before accessing a destination. In
 * particular, an error event may report cancellation before the reuse
 * dependency resolves and does not supersede that dependency. Queueing
 * reserves event space up front, so completion never allocates in the vblank
 * path.
 */
struct drm_castkms_capture_queue_buffer {
	__u32 stream_id;
	__u32 buffer_id;
	__u32 flags;
	__u32 reserved;
	__u64 user_data;
	__u64 mode_generation;
	__u64 ready_point;
	__u64 reuse_point;
};

/**
 * struct drm_castkms_capture_set_output_edid - publish an EDID for the captured output
 * @stream_id: file-local capture stream identifier
 * @flags: must be zero
 * @edid_size: EDID blob size in bytes; zero clears the published EDID
 * @reserved: must be zero
 * @edid_ptr: userspace pointer to @edid_size bytes, or zero when clearing
 *
 * The stream owner may push a complete EDID while this file owns an attached
 * monitor on the captured CRTC. The driver copies and validates the blob,
 * updates the connector, and emits a standard KMS hotplug so compositors
 * reread identity and modes. Call again when the sink identity changes. A
 * zero-length blob clears the published EDID without detaching. Stream stop
 * leaves the attachment and EDID in place. DETACH_MONITOR or file close
 * unplugs the monitor and clears the EDID.
 *
 * This ioctl is fire-and-forget. Capture completion events do not report
 * EDID changes; the client is the source of truth and already knows what it
 * wrote. Consumers observe the connector EDID property and KMS hotplug.
 *
 * When setting, @edid_size must be a non-zero multiple of 128 and at most
 * 512. Invalid EDIDs return -EINVAL. A stream that is not owned by this file
 * returns -ENOENT. A connector that is not attached returns -ENOTCONN.
 * A connector attached by another file returns -EACCES.
 */
struct drm_castkms_capture_set_output_edid {
	__u32 stream_id;
	__u32 flags;
	__u32 edid_size;
	__u32 reserved;
	__u64 edid_ptr;
};

/**
 * struct drm_castkms_capture_attach_monitor - plug a sink into a connector
 * @connector_id: DRM object ID of the display connector
 * @flags: must be zero
 * @edid_size: EDID blob size in bytes; zero attaches without an EDID
 * @reserved: must be zero
 * @edid_ptr: userspace pointer to @edid_size bytes, or zero when no EDID
 *
 * The default device publishes a fixed set of disconnected virtual ports at
 * load. This ioctl is the plug-in: the connector becomes connected, the
 * optional EDID is published, and a standard KMS hotplug is emitted. The
 * calling file owns the attachment until DETACH_MONITOR or close.
 *
 * When setting an EDID, @edid_size must be a non-zero multiple of 128 and
 * at most 512. Invalid EDIDs return -EINVAL. A writeback or unknown
 * connector returns -ENOENT. A connector already attached by any file
 * returns -EBUSY.
 */
struct drm_castkms_capture_attach_monitor {
	__u32 connector_id;
	__u32 flags;
	__u32 edid_size;
	__u32 reserved;
	__u64 edid_ptr;
};

/**
 * struct drm_castkms_capture_detach_monitor - unplug a sink from a connector
 * @connector_id: DRM object ID of the display connector
 * @flags: must be zero
 * @reserved: must be zero
 *
 * Clears the published EDID, marks the connector disconnected, and emits a
 * standard KMS hotplug. Only the attaching file may detach. A connector
 * that is not attached returns -ENOTCONN. A connector attached by another
 * file returns -EACCES.
 */
struct drm_castkms_capture_detach_monitor {
	__u32 connector_id;
	__u32 flags;
	__u64 reserved;
};

/**
 * DRM_CASTKMS_CAPTURE_EVENT_FRAME:
 *
 * Driver-private event type carrying a completed capture frame.
 */
#define DRM_CASTKMS_CAPTURE_EVENT_FRAME	0x80000000U

/**
 * DRM_CASTKMS_CAPTURE_FRAME_FULL_DAMAGE:
 *
 * The damage rectangle covers the complete captured frame.
 */
#define DRM_CASTKMS_CAPTURE_FRAME_FULL_DAMAGE	(1U << 0)

/**
 * DRM_CASTKMS_CAPTURE_FRAME_MODE_CHANGED:
 *
 * The queued buffer was returned because its mode generation became stale.
 * The stream remains bound to its original generation; stop it and start a
 * new stream before registering or queueing buffers for the new mode.
 */
#define DRM_CASTKMS_CAPTURE_FRAME_MODE_CHANGED	(1U << 1)

/**
 * DRM_CASTKMS_CURSOR_VISIBLE:
 *
 * The cursor plane was visible when the frame was captured.
 */
#define DRM_CASTKMS_CURSOR_VISIBLE		(1U << 0)

/**
 * DRM_CASTKMS_CURSOR_IMAGE_CHANGED:
 *
 * The cursor image, hotspot, or visibility changed since the previous
 * successful capture on this stream. The client should re-fetch the bitmap
 * from the buffer named by the event and cache it for subsequent stream
 * events.
 */
#define DRM_CASTKMS_CURSOR_IMAGE_CHANGED	(1U << 1)

/**
 * struct drm_event_castkms_capture_frame - capture completion event
 * @base: DRM event header with type DRM_CASTKMS_CAPTURE_EVENT_FRAME
 * @user_data: opaque value supplied when the buffer was queued
 * @sequence: vblank sequence associated with the frame, or zero on cancellation
 * @timestamp_ns: vblank timestamp for a captured frame, or monotonic
 *                completion time when @sequence is zero
 * @mode_generation: generation used for this completion
 * @stream_id: file-local capture stream identifier
 * @buffer_id: stream-local registered buffer identifier
 * @status: zero on success or a negative errno on asynchronous failure
 * @flags: bitmask of DRM_CASTKMS_CAPTURE_FRAME_* values
 * @dropped_frames: frames skipped since this buffer was queued
 * @damage_x: left edge of the changed rectangle
 * @damage_y: top edge of the changed rectangle
 * @damage_width: width of the changed rectangle
 * @damage_height: height of the changed rectangle
 * @cursor_serial: cursor generation counter; 0 means no cursor data
 * @cursor_flags: bitmask of DRM_CASTKMS_CURSOR_* values
 * @cursor_x: cursor screen X position (valid when CURSOR_VISIBLE)
 * @cursor_y: cursor screen Y position (valid when CURSOR_VISIBLE)
 * @cursor_hotspot_x: hotspot X offset within cursor image
 * @cursor_hotspot_y: hotspot Y offset within cursor image
 * @cursor_width: cursor image width in pixels
 * @cursor_height: cursor image height in pixels
 * @reserved: must be zero
 */
struct drm_event_castkms_capture_frame {
	struct drm_event base;
	__u64 user_data;
	__u64 sequence;
	__s64 timestamp_ns;
	__u64 mode_generation;
	__u32 stream_id;
	__u32 buffer_id;
	__s32 status;
	__u32 flags;
	__u32 dropped_frames;
	__s32 damage_x;
	__s32 damage_y;
	__u32 damage_width;
	__u32 damage_height;
	__u32 cursor_serial;
	__u32 cursor_flags;
	__s32 cursor_x;
	__s32 cursor_y;
	__u32 cursor_hotspot_x;
	__u32 cursor_hotspot_y;
	__u32 cursor_width;
	__u32 cursor_height;
	__u32 reserved;
};

#define DRM_CASTKMS_CAPTURE_QUERY_CAPS	0x00
#define DRM_CASTKMS_CAPTURE_START	0x01
#define DRM_CASTKMS_CAPTURE_STOP		0x02
#define DRM_CASTKMS_CAPTURE_REGISTER_BUFFER	0x03
#define DRM_CASTKMS_CAPTURE_UNREGISTER_BUFFER	0x04
#define DRM_CASTKMS_CAPTURE_QUEUE_BUFFER	0x05
#define DRM_CASTKMS_CAPTURE_SET_OUTPUT_EDID	0x06
#define DRM_CASTKMS_CAPTURE_ATTACH_MONITOR	0x07
#define DRM_CASTKMS_CAPTURE_DETACH_MONITOR	0x08

#define DRM_IOCTL_CASTKMS_CAPTURE_QUERY_CAPS \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_CASTKMS_CAPTURE_QUERY_CAPS, \
		 struct drm_castkms_capture_query_caps)
#define DRM_IOCTL_CASTKMS_CAPTURE_START \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_CASTKMS_CAPTURE_START, \
		 struct drm_castkms_capture_start)
#define DRM_IOCTL_CASTKMS_CAPTURE_STOP \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CASTKMS_CAPTURE_STOP, \
		struct drm_castkms_capture_stop)
#define DRM_IOCTL_CASTKMS_CAPTURE_REGISTER_BUFFER \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_CASTKMS_CAPTURE_REGISTER_BUFFER, \
		 struct drm_castkms_capture_register_buffer)
#define DRM_IOCTL_CASTKMS_CAPTURE_UNREGISTER_BUFFER \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CASTKMS_CAPTURE_UNREGISTER_BUFFER, \
		struct drm_castkms_capture_unregister_buffer)
#define DRM_IOCTL_CASTKMS_CAPTURE_QUEUE_BUFFER \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CASTKMS_CAPTURE_QUEUE_BUFFER, \
		struct drm_castkms_capture_queue_buffer)
#define DRM_IOCTL_CASTKMS_CAPTURE_SET_OUTPUT_EDID \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CASTKMS_CAPTURE_SET_OUTPUT_EDID, \
		struct drm_castkms_capture_set_output_edid)
#define DRM_IOCTL_CASTKMS_CAPTURE_ATTACH_MONITOR \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CASTKMS_CAPTURE_ATTACH_MONITOR, \
		struct drm_castkms_capture_attach_monitor)
#define DRM_IOCTL_CASTKMS_CAPTURE_DETACH_MONITOR \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CASTKMS_CAPTURE_DETACH_MONITOR, \
		struct drm_castkms_capture_detach_monitor)

#if defined(__cplusplus)
}
#endif

#endif /* _UAPI_CASTKMS_DRM_H_ */
