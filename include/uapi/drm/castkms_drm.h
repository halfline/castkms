/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */

#ifndef _UAPI_CASTKMS_DRM_H_
#define _UAPI_CASTKMS_DRM_H_

#include <drm/drm.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define DRM_CASTKMS_CAPTURE_UAPI_MAJOR	0
#define DRM_CASTKMS_CAPTURE_UAPI_MINOR	9

/* Immutable capture-protocol limits shared by the driver and clients. */
#define DRM_CASTKMS_CAPTURE_MIN_WIDTH		10U
#define DRM_CASTKMS_CAPTURE_MIN_HEIGHT		10U
#define DRM_CASTKMS_CAPTURE_MAX_WIDTH		8192U
#define DRM_CASTKMS_CAPTURE_MAX_HEIGHT		8192U
#define DRM_CASTKMS_CAPTURE_MAX_CURSOR_WIDTH	512U
#define DRM_CASTKMS_CAPTURE_MAX_CURSOR_HEIGHT	512U
#define DRM_CASTKMS_CAPTURE_MAX_EDID_SIZE	512U

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
 * Permit monitor attachment for the connector named by the grant.
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
 * Permit cursor inclusion in a capture stream.
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
 * The current top-level owner master may revoke a delegated grant bound to
 * that exact drm_master, including after its privileged creator has closed.
 * A caller with CAP_SYS_ADMIN in the initial user namespace may revoke any
 * live grant on the device.
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
 * A holder queries its own grant with @grant_id zero. A revoker file queries a
 * grant by ID. The current top-level owner master may query a delegated grant
 * bound to its exact drm_master, and initial-user-namespace CAP_SYS_ADMIN may
 * query any live grant by ID; another ordinary DRM file returns -ENODATA. A
 * normal or delegated suspended grant becomes active again whenever its bound
 * DRM master returns and reestablishes a capture-safe composition, even after
 * an intervening owner. A delegated holder reports
 * DRM_CASTKMS_GRANT_FLAG_DELEGATED. Mode-specific streams must still be
 * recreated after any master loss. A revoked grant remains queryable but can
 * never regain authority.
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
 * struct drm_castkms_capture_start - start an exclusive capture stream
 * @crtc_id: DRM object ID of the CRTC to observe
 * @flags: must be DRM_CASTKMS_CAPTURE_START_EXCLUSIVE
 * @stream_id: file-local stream identifier returned by the driver
 * @reserved: must be zero
 * @mode_generation: current CRTC mode generation returned by the driver
 *
 * Starting capture does not activate or otherwise change the selected CRTC.
 * Only one live capture stream may own a CRTC at a time.
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
 * struct drm_castkms_capture_register_buffer - register a capture destination
 * @stream_id: file-local capture stream identifier
 * @fb_id: framebuffer object ID visible to this DRM file
 * @flags: must be DRM_CASTKMS_CAPTURE_BUFFER_IMPLICIT_SYNC
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

#define DRM_CASTKMS_CAPTURE_QUERY_CAPS	0x00
#define DRM_CASTKMS_CAPTURE_START	0x01
#define DRM_CASTKMS_CAPTURE_STOP		0x02
#define DRM_CASTKMS_CAPTURE_REGISTER_BUFFER	0x03
#define DRM_CASTKMS_CAPTURE_UNREGISTER_BUFFER	0x04
#define DRM_CASTKMS_CREATE_GRANT			0x11
#define DRM_CASTKMS_REVOKE_GRANT			0x12
#define DRM_CASTKMS_GET_GRANT			0x13

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
#define DRM_IOCTL_CASTKMS_CREATE_GRANT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_CASTKMS_CREATE_GRANT, \
		 struct drm_castkms_create_grant)
#define DRM_IOCTL_CASTKMS_REVOKE_GRANT \
	DRM_IOW(DRM_COMMAND_BASE + DRM_CASTKMS_REVOKE_GRANT, \
		struct drm_castkms_revoke_grant)
#define DRM_IOCTL_CASTKMS_GET_GRANT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_CASTKMS_GET_GRANT, \
		 struct drm_castkms_get_grant)

#if defined(__cplusplus)
}
#endif

#endif /* _UAPI_CASTKMS_DRM_H_ */
