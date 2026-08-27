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
CASTKMS_PRIVATE_IOCTL(CASTKMS_CAPTURE_START,
			     castkms_capture_start_ioctl, ALLOWED)
CASTKMS_PRIVATE_IOCTL(CASTKMS_CAPTURE_STOP,
			     castkms_capture_stop_ioctl, ALLOWED)
CASTKMS_PRIVATE_IOCTL(CASTKMS_CAPTURE_REGISTER_BUFFER,
			     castkms_capture_register_buffer_ioctl, ALLOWED)
CASTKMS_PRIVATE_IOCTL(CASTKMS_CAPTURE_UNREGISTER_BUFFER,
			     castkms_capture_unregister_buffer_ioctl, ALLOWED)
CASTKMS_PRIVATE_IOCTL(CASTKMS_CAPTURE_QUEUE_BUFFER,
			     castkms_capture_queue_buffer_ioctl, ALLOWED)
CASTKMS_PRIVATE_IOCTL(CASTKMS_CAPTURE_SET_OUTPUT_EDID,
			     castkms_capture_set_output_edid_ioctl, ALLOWED)
CASTKMS_PRIVATE_IOCTL(CASTKMS_CAPTURE_ATTACH_MONITOR,
			     castkms_capture_attach_monitor_ioctl, ALLOWED)
CASTKMS_PRIVATE_IOCTL(CASTKMS_CAPTURE_DETACH_MONITOR,
			     castkms_capture_detach_monitor_ioctl, ALLOWED)
CASTKMS_PRIVATE_IOCTL(CASTKMS_CAPTURE_READ_CURSOR_BITMAP,
			     castkms_capture_read_cursor_bitmap_ioctl, ALLOWED)
CASTKMS_PRIVATE_IOCTL(CASTKMS_CREATE_GRANT,
			     castkms_grant_create_ioctl, DENIED)
CASTKMS_PRIVATE_IOCTL(CASTKMS_REVOKE_GRANT,
			     castkms_grant_revoke_ioctl, DENIED)
CASTKMS_PRIVATE_IOCTL(CASTKMS_GET_GRANT,
			     castkms_grant_get_ioctl, ALLOWED)
CASTKMS_PRIVATE_IOCTL(CASTKMS_GET_OUTPUT,
			     castkms_get_output_ioctl, DENIED)

#ifdef CASTKMS_HAVE_CEC
CASTKMS_PRIVATE_IOCTL(CASTKMS_CEC_QUERY_CAPS,
			     castkms_cec_query_caps_ioctl, ALLOWED)
#endif
