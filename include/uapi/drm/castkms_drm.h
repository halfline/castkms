/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */

#ifndef _UAPI_CASTKMS_DRM_H_
#define _UAPI_CASTKMS_DRM_H_

#include <drm/drm.h>

#if defined(__cplusplus)
extern "C" {
#endif

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

#define DRM_CASTKMS_CREATE_GRANT			0x11
#define DRM_CASTKMS_REVOKE_GRANT			0x12
#define DRM_CASTKMS_GET_GRANT			0x13

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
