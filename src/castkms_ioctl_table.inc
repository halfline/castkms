/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * name, handler, grant-file access
 *
 * This is the single registry for CastKMS private ioctl dispatch and the
 * security-sensitive grant-file allowlist. Keep policy in the third column:
 * ALLOWED or DENIED. The includer must define CASTKMS_PRIVATE_IOCTL().
 */
CASTKMS_PRIVATE_IOCTL(CASTKMS_CAPTURE_QUERY_CAPS,
			     castkms_capture_query_caps_ioctl, ALLOWED)
CASTKMS_PRIVATE_IOCTL(CASTKMS_CREATE_GRANT,
			     castkms_grant_create_ioctl, DENIED)
CASTKMS_PRIVATE_IOCTL(CASTKMS_REVOKE_GRANT,
			     castkms_grant_revoke_ioctl, DENIED)
CASTKMS_PRIVATE_IOCTL(CASTKMS_GET_GRANT,
			     castkms_grant_get_ioctl, ALLOWED)
