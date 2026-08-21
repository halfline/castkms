/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */

#ifndef _UAPI_CASTKMS_DRM_H_
#define _UAPI_CASTKMS_DRM_H_

#include <drm/drm.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define DRM_CASTKMS_CAPTURE_UAPI_MAJOR	0
#define DRM_CASTKMS_CAPTURE_UAPI_MINOR	9

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
 * DRM_CASTKMS_CAPTURE_CAP_GRANT_FD:
 *
 * Sensitive capture, monitor-management, and CEC operations require a
 * connector-scoped grant-bearing DRM file descriptor. Opening the primary
 * node does not confer those rights.
 */
#define DRM_CASTKMS_CAPTURE_CAP_GRANT_FD		(1ULL << 3)

/**
 * DRM_CASTKMS_GRANT_CAPTURE_PIXELS:
 *
 * Permit capture stream creation and buffer operations for the connector
 * named by the grant. Cursor access additionally requires
 * DRM_CASTKMS_GRANT_READ_CURSOR.
 */
#define DRM_CASTKMS_GRANT_CAPTURE_PIXELS	(1U << 0)

/**
 * DRM_CASTKMS_GRANT_MANAGE_ATTACHMENT:
 *
 * Permit monitor attachment and detachment for the connector named by the
 * grant.
 */
#define DRM_CASTKMS_GRANT_MANAGE_ATTACHMENT	(1U << 1)

/**
 * DRM_CASTKMS_GRANT_UPDATE_EDID:
 *
 * Permit publishing or clearing the connector's EDID.
 */
#define DRM_CASTKMS_GRANT_UPDATE_EDID		(1U << 2)

/**
 * DRM_CASTKMS_GRANT_READ_CURSOR:
 *
 * Permit cursor inclusion, metadata, and bitmap retrieval in a capture
 * stream. Pixel capture without this right must exclude the cursor.
 */
#define DRM_CASTKMS_GRANT_READ_CURSOR		(1U << 3)

/**
 * DRM_CASTKMS_GRANT_MANAGE_CEC:
 *
 * Permit ownership and use of the userspace CEC transport for the connector
 * named by the grant.
 */
#define DRM_CASTKMS_GRANT_MANAGE_CEC		(1U << 4)

#define DRM_CASTKMS_GRANT_RIGHTS_MASK \
	(DRM_CASTKMS_GRANT_CAPTURE_PIXELS | \
	 DRM_CASTKMS_GRANT_MANAGE_ATTACHMENT | \
	 DRM_CASTKMS_GRANT_UPDATE_EDID | \
	 DRM_CASTKMS_GRANT_READ_CURSOR | \
	 DRM_CASTKMS_GRANT_MANAGE_CEC)

/**
 * DRM_CASTKMS_GRANT_CREATE_ADMIN:
 *
 * Create a master-independent administrative grant. This flag requires
 * CAP_SYS_ADMIN in the initial user namespace. An administrative grant is not
 * suspended merely because DRM master changes, but pixel capture still waits
 * until the current master owns capture-safe content on the connector.
 */
#define DRM_CASTKMS_GRANT_CREATE_ADMIN		(1U << 0)

/**
 * DRM_CASTKMS_GRANT_CREATE_DELEGATED:
 *
 * Create a holder-lived normal grant bound to the device's current top-level
 * DRM owner master. This flag requires CAP_SYS_ADMIN in the initial user
 * namespace and cannot be combined with DRM_CASTKMS_GRANT_CREATE_ADMIN. The
 * caller must not itself be a current DRM master. Closing the creating file
 * does not revoke a delegated grant.
 */
#define DRM_CASTKMS_GRANT_CREATE_DELEGATED	(1U << 1)

#define DRM_CASTKMS_GRANT_CREATE_FLAGS_MASK \
	(DRM_CASTKMS_GRANT_CREATE_ADMIN | \
	 DRM_CASTKMS_GRANT_CREATE_DELEGATED)

/** DRM_CASTKMS_GRANT_FLAG_ADMIN: Grant has roaming administrative semantics. */
#define DRM_CASTKMS_GRANT_FLAG_ADMIN		(1U << 0)
/** DRM_CASTKMS_GRANT_FLAG_DELEGATED: Grant is holder-lived and master-bound. */
#define DRM_CASTKMS_GRANT_FLAG_DELEGATED	(1U << 1)
#define DRM_CASTKMS_GRANT_FLAGS_MASK \
	(DRM_CASTKMS_GRANT_FLAG_ADMIN | DRM_CASTKMS_GRANT_FLAG_DELEGATED)

/**
 * struct drm_castkms_create_grant - create a connector-scoped capability fd
 * @connector_id: DRM object ID of a non-writeback CastKMS connector
 * @rights: nonzero mask of DRM_CASTKMS_GRANT_* rights
 * @flags: DRM_CASTKMS_GRANT_CREATE_* flags
 * @fd: output file descriptor carrying the grant
 * @grant_id: output device-unique grant identifier
 * @fd_flags: flags for the returned file; only O_NONBLOCK is accepted
 *
 * With no creation flags, this operation requires the device's current
 * top-level DRM owner master and creates a normal grant revoked when that
 * creating file closes. A DRM lease master cannot create such a grant.
 * DRM_CASTKMS_GRANT_CREATE_DELEGATED requires CAP_SYS_ADMIN in the initial
 * user namespace and creates a holder-lived normal grant bound to the current
 * owner master; it returns -EAGAIN when the caller is current or no owner
 * master exists. DRM_CASTKMS_GRANT_CREATE_ADMIN requires the same capability
 * and explicitly asks for master-independent administrative semantics. The
 * two creation flags are mutually exclusive.
 * The returned file has a fresh DRM namespace, is neither master nor
 * authenticated, and may be passed with SCM_RIGHTS. A normal or delegated
 * grant is suspended while another DRM master is current and revivifies when
 * its bound master returns and owns capture-safe connector content. An
 * administrative grant follows current safe content across master changes.
 * Closing the final holder reference permanently revokes every grant. Closing
 * the creating file also revokes normal and administrative grants, but not a
 * delegated grant. The kernel always creates @fd with close-on-exec set,
 * independently of @fd_flags.
 */
struct drm_castkms_create_grant {
	__u32 connector_id;
	__u32 rights;
	__u32 flags;
	__s32 fd;
	__u32 grant_id;
	__u32 fd_flags;
};

/**
 * struct drm_castkms_revoke_grant - permanently revoke a live grant
 * @grant_id: identifier returned by DRM_IOCTL_CASTKMS_CREATE_GRANT
 * @flags: must be zero
 * @reserved: must be zero
 *
 * A grant's revoker file may revoke it even while its DRM master is inactive.
 * A caller with CAP_SYS_ADMIN in the initial user namespace may revoke any
 * live grant on the device, including a delegated grant after its creator has
 * closed.
 */
struct drm_castkms_revoke_grant {
	__u32 grant_id;
	__u32 flags;
	__u64 reserved;
};

#define DRM_CASTKMS_GRANT_STATE_PENDING			0
#define DRM_CASTKMS_GRANT_STATE_ACTIVE			1
#define DRM_CASTKMS_GRANT_STATE_SUSPENDED_NO_MASTER	2
#define DRM_CASTKMS_GRANT_STATE_SUSPENDED_OTHER_MASTER	3
#define DRM_CASTKMS_GRANT_STATE_SUSPENDED_FOREIGN_CONTENT 4
#define DRM_CASTKMS_GRANT_STATE_REVOKED			5

/**
 * struct drm_castkms_get_grant - query a held or revocable grant
 * @grant_id: holder input zero/output identity; revoker input grant identifier
 * @connector_id: output authorized connector ID
 * @rights: output DRM_CASTKMS_GRANT_* mask
 * @state: output DRM_CASTKMS_GRANT_STATE_* value
 * @flags: must be zero on input; output DRM_CASTKMS_GRANT_FLAG_* mask
 * @reserved: must be zero on input and is zero on output
 * @reserved2: must be zero on input and is zero on output
 *
 * An ordinary DRM file returns -ENODATA. A normal or delegated suspended grant
 * becomes active again whenever its bound DRM master returns and reestablishes
 * a capture-safe composition, even after an intervening owner. A delegated
 * holder reports DRM_CASTKMS_GRANT_FLAG_DELEGATED. Mode-specific streams must
 * still be recreated after any master loss. A revoked grant remains queryable
 * but can never regain authority.
 */
struct drm_castkms_get_grant {
	__u32 grant_id;
	__u32 connector_id;
	__u32 rights;
	__u32 state;
	__u32 flags;
	__u32 reserved;
	__u64 reserved2;
};

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
 * Exclude the cursor plane from captured frame composition. With
 * DRM_CASTKMS_GRANT_READ_CURSOR, position and image metadata are still
 * reported in capture events so consumers can render the cursor client-side.
 * Without that right all cursor fields and bitmaps are suppressed.
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
 * @connector_id: DRM object ID of the grant's connector
 * @flags: must be zero
 * @edid_size: EDID blob size in bytes; zero clears the published EDID
 * @reserved: must be zero
 * @edid_ptr: userspace pointer to @edid_size bytes, or zero when clearing
 *
 * A grant with DRM_CASTKMS_GRANT_UPDATE_EDID may push a complete EDID while
 * it owns an attachment on this connector. The driver copies and validates the
 * blob, updates the connector, and emits a standard KMS hotplug so compositors
 * reread identity and modes. Call again when the sink identity changes. A
 * zero-length blob clears the published EDID without detaching. Stream stop
 * leaves the attachment and EDID in place. DETACH_MONITOR or grant revocation
 * unplugs the monitor and clears the EDID.
 *
 * This ioctl is fire-and-forget. Capture completion events do not report
 * EDID changes; the client is the source of truth and already knows what it
 * wrote. Consumers observe the connector EDID property and KMS hotplug.
 *
 * When setting, @edid_size must be a non-zero multiple of 128 and at most
 * 512. Invalid EDIDs return -EINVAL. A connector that is not attached returns
 * -ENOTCONN. A connector attached by another grant returns -EACCES.
 */
struct drm_castkms_capture_set_output_edid {
	__u32 connector_id;
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
 * calling grant owns the attachment until DETACH_MONITOR, revocation, or final
 * holder close.
 *
 * When setting an EDID, @edid_size must be a non-zero multiple of 128 and
 * at most 512. Invalid EDIDs return -EINVAL. A writeback or unknown
 * connector returns -ENOENT. A connector already attached by any grant
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
 * standard KMS hotplug. Only the attachment-owning grant may detach. A
 * connector that is not attached returns -ENOTCONN. A connector attached by
 * another grant returns -EACCES.
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
 * DRM_CASTKMS_CAPTURE_EVENT_GRANT_REVOKED:
 *
 * Reliable notification that a grant has become permanently inert. This is
 * intentionally distinct from a mode-generation change, after which the same
 * grant may start a replacement stream.
 */
#define DRM_CASTKMS_CAPTURE_EVENT_GRANT_REVOKED	0x80000003U
#define DRM_CASTKMS_CAPTURE_EVENT_GRANT_STATE	0x80000004U

/**
 * struct drm_event_castkms_grant_revoked - grant revocation notification
 * @base: event header with type DRM_CASTKMS_CAPTURE_EVENT_GRANT_REVOKED
 * @grant_id: revoked grant identifier
 * @status: -EKEYREVOKED for policy or lifetime revoke; -ENODEV for teardown
 * @timestamp_ns: monotonic revocation timestamp
 */
struct drm_event_castkms_grant_revoked {
	struct drm_event base;
	__u32 grant_id;
	__s32 status;
	__u64 timestamp_ns;
};

/**
 * struct drm_event_castkms_grant_state - non-terminal grant state change
 * @base: event header with type DRM_CASTKMS_CAPTURE_EVENT_GRANT_STATE
 * @grant_id: grant identifier
 * @state: new DRM_CASTKMS_GRANT_STATE_* value
 * @status: errno returned by pixel-capture operations in this state
 * @reserved: zero
 * @timestamp_ns: monotonic transition timestamp
 *
 * State events are advisory. Userspace must use GET_GRANT as the
 * authoritative state if transitions coalesce or event reservation fails.
 */
struct drm_event_castkms_grant_state {
	struct drm_event base;
	__u32 grant_id;
	__u32 state;
	__s32 status;
	__u32 reserved;
	__u64 timestamp_ns;
};

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
#define DRM_CASTKMS_CAPTURE_READ_CURSOR_BITMAP	0x09
#define DRM_CASTKMS_CREATE_GRANT			0x11
#define DRM_CASTKMS_REVOKE_GRANT			0x12
#define DRM_CASTKMS_GET_GRANT			0x13

/**
 * struct drm_castkms_capture_read_cursor_bitmap - read cursor image data
 * @stream_id: file-local capture stream identifier
 * @buffer_id: read cursor snapshot from this buffer (must be IDLE)
 * @format: output: DRM_FORMAT_ARGB8888
 * @width: output: cursor image width in pixels
 * @height: output: cursor image height in pixels
 * @stride: output: bytes per row
 * @bitmap_size: input: buffer capacity in bytes; output: required size
 * @reserved: must be zero
 * @bitmap_ptr: input: userspace buffer for pixel data
 *
 * Reads the cursor image that was snapshotted during composition of the
 * specified capture buffer.  Call after the capture event and before
 * re-queuing.  Only needed when DRM_CASTKMS_CURSOR_IMAGE_CHANGED is set.
 */
struct drm_castkms_capture_read_cursor_bitmap {
	__u32 stream_id;
	__u32 buffer_id;
	__u32 format;
	__u32 width;
	__u32 height;
	__u32 stride;
	__u32 bitmap_size;
	__u32 reserved;
	__u64 bitmap_ptr;
};

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
#define DRM_IOCTL_CASTKMS_CAPTURE_READ_CURSOR_BITMAP \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_CASTKMS_CAPTURE_READ_CURSOR_BITMAP, \
		 struct drm_castkms_capture_read_cursor_bitmap)
#define DRM_IOCTL_CASTKMS_CREATE_GRANT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_CASTKMS_CREATE_GRANT, \
		 struct drm_castkms_create_grant)
#define DRM_IOCTL_CASTKMS_REVOKE_GRANT \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CASTKMS_REVOKE_GRANT, \
		struct drm_castkms_revoke_grant)
#define DRM_IOCTL_CASTKMS_GET_GRANT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_CASTKMS_GET_GRANT, \
		 struct drm_castkms_get_grant)

/* --- CEC transport UAPI --- */

#define DRM_CASTKMS_CEC_UAPI_MAJOR	0
#define DRM_CASTKMS_CEC_UAPI_MINOR	1

/* CEC capability flags */
#define DRM_CASTKMS_CEC_CAP_ASYNC_TX		(1ULL << 0)
#define DRM_CASTKMS_CEC_CAP_RX_INJECT		(1ULL << 1)
#define DRM_CASTKMS_CEC_CAP_STATE_EVENTS	(1ULL << 2)
#define DRM_CASTKMS_CEC_CAP_TRANSPORT_STATE	(1ULL << 3)
#define DRM_CASTKMS_CEC_CAP_EDID_PHYS_ADDR	(1ULL << 4)

/* Transport state flags for CEC_SET_TRANSPORT_STATE */
#define DRM_CASTKMS_CEC_TRANSPORT_ONLINE	(1U << 0)

/* State flags for bind response and get-state */
#define DRM_CASTKMS_CEC_STATE_TRANSPORT_ONLINE	(1U << 0)
#define DRM_CASTKMS_CEC_STATE_MONITOR_ATTACHED	(1U << 1)
#define DRM_CASTKMS_CEC_STATE_ADAPTER_ENABLED	(1U << 2)

/**
 * struct drm_castkms_cec_query_caps - query CEC capabilities for a connector
 * @connector_id: DRM object ID of the display connector
 * @flags: must be zero
 * @uapi_major: CEC UAPI major version returned by the driver
 * @uapi_minor: CEC UAPI minor version returned by the driver
 * @capabilities: bitmask of DRM_CASTKMS_CEC_CAP_* values
 * @max_msg_size: maximum CEC message size (16)
 * @output_index: stable castkms output identity
 * @has_adapter: 1 if a CEC adapter is registered for this connector
 * @reserved: must be zero
 */
struct drm_castkms_cec_query_caps {
	__u32 connector_id;
	__u32 flags;
	__u32 uapi_major;
	__u32 uapi_minor;
	__u64 capabilities;
	__u32 max_msg_size;
	__u32 output_index;
	__u32 has_adapter;
	__u32 reserved;
};

/**
 * struct drm_castkms_cec_bind_transport - bind as the CEC transport owner
 * @connector_id: DRM object ID of the display connector
 * @flags: must be zero
 * @transport_id: file-local transport ID returned by the driver
 * @reserved: must be zero
 * @transport_generation: transport generation returned by the driver
 * @state_generation: current state generation returned by the driver
 * @output_index: stable castkms output identity
 * @state_flags: bitmask of DRM_CASTKMS_CEC_STATE_* values
 * @phys_addr: current EDID-derived physical address
 * @logical_addr_mask: current logical address mask
 */
struct drm_castkms_cec_bind_transport {
	__u32 connector_id;
	__u32 flags;
	__u32 transport_id;
	__u32 reserved;
	__u64 transport_generation;
	__u64 state_generation;
	__u32 output_index;
	__u32 state_flags;
	__u16 phys_addr;
	__u16 logical_addr_mask;
};

/**
 * struct drm_castkms_cec_unbind_transport - release transport ownership
 * @connector_id: DRM object ID of the display connector
 * @transport_id: file-local transport ID from bind
 * @flags: must be zero
 * @reserved: must be zero
 */
struct drm_castkms_cec_unbind_transport {
	__u32 connector_id;
	__u32 transport_id;
	__u32 flags;
	__u32 reserved;
};

/**
 * struct drm_castkms_cec_set_transport_state - set online/offline state
 * @connector_id: DRM object ID of the display connector
 * @transport_id: file-local transport ID from bind
 * @flags: DRM_CASTKMS_CEC_TRANSPORT_ONLINE or 0 for offline
 * @reserved: must be zero
 */
struct drm_castkms_cec_set_transport_state {
	__u32 connector_id;
	__u32 transport_id;
	__u32 flags;
	__u32 reserved;
};

/**
 * struct drm_castkms_cec_tx_complete - complete an outbound CEC transaction
 * @connector_id: DRM object ID of the display connector
 * @transport_id: file-local transport ID from bind
 * @transport_generation: generation from the TX event
 * @cookie: transaction cookie from the TX event
 * @status: Linux CEC transmit status (CEC_TX_STATUS_*)
 * @arb_lost_cnt: arbitration-lost counter
 * @nack_cnt: NACK counter
 * @low_drive_cnt: low-drive counter
 * @error_cnt: generic error counter
 * @reserved: must be zero
 */
struct drm_castkms_cec_tx_complete {
	__u32 connector_id;
	__u32 transport_id;
	__u64 transport_generation;
	__u64 cookie;
	__u8 status;
	__u8 arb_lost_cnt;
	__u8 nack_cnt;
	__u8 low_drive_cnt;
	__u8 error_cnt;
	__u8 reserved[3];
};

/**
 * struct drm_castkms_cec_receive - inject a received CEC message
 * @connector_id: DRM object ID of the display connector
 * @transport_id: file-local transport ID from bind
 * @transport_generation: generation from bind
 * @length: message length (1-16)
 * @flags: must be zero
 * @msg: CEC message bytes
 * @reserved: must be zero
 */
struct drm_castkms_cec_receive {
	__u32 connector_id;
	__u32 transport_id;
	__u64 transport_generation;
	__u8 length;
	__u8 flags;
	__u8 msg[16];
	__u8 reserved;
};

/**
 * struct drm_castkms_cec_get_state - get authoritative transport state
 * @connector_id: DRM object ID of the display connector
 * @transport_id: file-local transport ID from bind
 * @flags: must be zero
 * @reserved: must be zero
 * @transport_generation: current transport generation
 * @state_generation: current state generation
 * @state_flags: bitmask of DRM_CASTKMS_CEC_STATE_* values
 * @output_index: stable castkms output identity
 * @phys_addr: current physical address
 * @logical_addr_mask: current logical address mask
 * @pending_cookie: outstanding transaction cookie, or 0
 * @stats_tx_submitted: total transmit events delivered
 * @stats_tx_completed: total successful completions
 * @stats_tx_nack: total NACK completions
 * @stats_tx_error: total error completions
 * @stats_tx_timeout: total timeout completions
 * @stats_rx: total received messages injected
 * @stats_invalid: total rejected invalid requests
 */
struct drm_castkms_cec_get_state {
	__u32 connector_id;
	__u32 transport_id;
	__u32 flags;
	__u32 reserved;
	__u64 transport_generation;
	__u64 state_generation;
	__u32 state_flags;
	__u32 output_index;
	__u16 phys_addr;
	__u16 logical_addr_mask;
	__u64 pending_cookie;
	__u64 stats_tx_submitted;
	__u64 stats_tx_completed;
	__u64 stats_tx_nack;
	__u64 stats_tx_error;
	__u64 stats_tx_timeout;
	__u64 stats_rx;
	__u64 stats_invalid;
};

/* CEC DRM event types */
#define DRM_CASTKMS_CEC_EVENT_TX	0x80000001U
#define DRM_CASTKMS_CEC_EVENT_STATE	0x80000002U

/**
 * struct drm_castkms_cec_event_tx - outbound CEC transmit event
 * @base: DRM event header with type DRM_CASTKMS_CEC_EVENT_TX
 * @transport_id: file-local owner identifier
 * @transport_generation: detect stale bindings
 * @state_generation: associate request with adapter state
 * @cookie: match completion
 * @connector_id: direct connector identity
 * @output_index: stable A/V/C identity
 * @attempts: requested CEC attempt count
 * @signal_free_time: CEC-core timing request
 * @length: message length
 * @msg: up to 16 bytes of CEC message
 * @reserved: future extension
 */
struct drm_castkms_cec_event_tx {
	struct drm_event base;
	__u32 transport_id;
	__u32 pad0;
	__u64 transport_generation;
	__u64 state_generation;
	__u64 cookie;
	__u32 connector_id;
	__u32 output_index;
	__u8 attempts;
	__u8 length;
	__u8 msg[16];
	__u16 reserved;
	__u32 signal_free_time;
};

/* CEC ioctl command numbers (after capture range 0x00-0x09) */
#define DRM_CASTKMS_CEC_QUERY_CAPS		0x0a
#define DRM_CASTKMS_CEC_BIND_TRANSPORT		0x0b
#define DRM_CASTKMS_CEC_UNBIND_TRANSPORT	0x0c
#define DRM_CASTKMS_CEC_SET_TRANSPORT_STATE	0x0d
#define DRM_CASTKMS_CEC_TX_COMPLETE		0x0e
#define DRM_CASTKMS_CEC_RECEIVE			0x0f
#define DRM_CASTKMS_CEC_GET_STATE		0x10

#define DRM_IOCTL_CASTKMS_CEC_QUERY_CAPS \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_CASTKMS_CEC_QUERY_CAPS, \
		 struct drm_castkms_cec_query_caps)
#define DRM_IOCTL_CASTKMS_CEC_BIND_TRANSPORT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_CASTKMS_CEC_BIND_TRANSPORT, \
		 struct drm_castkms_cec_bind_transport)
#define DRM_IOCTL_CASTKMS_CEC_UNBIND_TRANSPORT \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CASTKMS_CEC_UNBIND_TRANSPORT, \
		struct drm_castkms_cec_unbind_transport)
#define DRM_IOCTL_CASTKMS_CEC_SET_TRANSPORT_STATE \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CASTKMS_CEC_SET_TRANSPORT_STATE, \
		struct drm_castkms_cec_set_transport_state)
#define DRM_IOCTL_CASTKMS_CEC_TX_COMPLETE \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CASTKMS_CEC_TX_COMPLETE, \
		struct drm_castkms_cec_tx_complete)
#define DRM_IOCTL_CASTKMS_CEC_RECEIVE \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CASTKMS_CEC_RECEIVE, \
		struct drm_castkms_cec_receive)
#define DRM_IOCTL_CASTKMS_CEC_GET_STATE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_CASTKMS_CEC_GET_STATE, \
		 struct drm_castkms_cec_get_state)

#if defined(__cplusplus)
}
#endif

#endif /* _UAPI_CASTKMS_DRM_H_ */
