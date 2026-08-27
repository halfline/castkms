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
 * struct drm_castkms_create_grant - create a connector-scoped capability fd
 * @connector_id: DRM object ID of a non-writeback CastKMS connector
 * @rights: nonzero mask of DRM_CASTKMS_GRANT_* rights
 * @flags: must be zero
 * @fd: output file descriptor carrying the grant
 * @grant_id: output device-unique grant identifier
 * @fd_flags: flags for the returned file; only O_NONBLOCK is accepted
 *
 * With no creation flags, this operation requires the device's current
 * top-level DRM owner master and creates a normal grant bound to it. A DRM
 * lease master cannot create such a grant.
 * The returned file has a fresh DRM namespace, is neither master nor
 * authenticated, and may be passed with SCM_RIGHTS. A normal grant is
 * suspended while another DRM master is current and revivifies when its bound
 * master returns and owns capture-safe connector content.
 * Closing the final holder reference permanently ends every grant. The
 * creating file has no association with the returned grant after this ioctl
 * completes. The kernel always creates @fd with close-on-exec set,
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

#define DRM_CASTKMS_CREATE_GRANT			0x11

#define DRM_IOCTL_CASTKMS_CREATE_GRANT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_CASTKMS_CREATE_GRANT, \
		 struct drm_castkms_create_grant)

#if defined(__cplusplus)
}
#endif

#endif /* _UAPI_CASTKMS_DRM_H_ */
