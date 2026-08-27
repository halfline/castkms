#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo_dir"

reject()
{
	local description=$1
	local pattern=$2
	shift 2

	if rg -n "$pattern" "$@"; then
		printf 'architecture dependency violation: %s\n' \
			"$description" >&2
		exit 1
	fi
}

reject 'output runtime header imports a complete subsystem API' \
	'#include "castkms_(capture|composer|frame)\.h"' \
	src/castkms_output.h

reject 'core device exposes grant-fd registry state' \
	'grant_registry|next_grant_id|struct xarray grants' \
	src/castkms_drv.h

reject 'grant adapter reaches into capture UAPI teardown' \
	'castkms_capture_uapi|stop_authority_stream' \
	src/castkms_grant.c src/castkms_grant.h

reject 'kernel limits import the public UAPI' \
	'castkms_drm\.h|DRM_CASTKMS_' \
	src/castkms_limits.h

reject 'pixel composer coordinates mutable frame consumers' \
	'castkms_capture|castkms_capture_owner|castkms_crtc|castkms_frame_dispatch|castkms_snapshot|drm_writeback|drm_crtc_add_crc' \
	src/castkms_composer.c src/castkms_composer.h

reject 'snapshot layer owns deferred capture execution' \
	'castkms_capture_queue_job|castkms_capture\.h|castkms_composer\.h|castkms_output\.h' \
	src/castkms_snapshot.c src/castkms_snapshot.h

reject 'stream scheduler owns buffer synchronization or cursor extraction' \
	'dma_resv|drm_syncobj|dma_fence_chain|drm_gem_fb_vmap|cursor_bitmap' \
	src/castkms_capture.c

reject 'buffer engine owns connector or CRTC scheduling' \
	'castkms_connector|castkms_crtc|drmm_mutex_init|castkms_capture_prepare_frame|castkms_capture_stream_(attach|destroy)' \
	src/castkms_capture_buffer.c

reject 'capture stream UAPI owns connector attachment translation' \
	'castkms_capture_(set_output_edid|attach_monitor|detach_monitor)_ioctl|drm_edid' \
	src/castkms_capture_uapi.c src/castkms_capture_uapi.h

reject 'ownership tracker imports authority policy' \
	'castkms_capture_authority|reconcile_ownership' \
	src/castkms_capture_owner.c src/castkms_capture_owner.h

reject 'driver carries a hand-written DRM-core grant allowlist' \
	'case DRM_IOCTL_[A-Z0-9_]+:' \
	src/castkms_drv.c

rg -q '^castkms_colorop_snapshot_init\(' src/castkms_colorop.c
reject 'plane layer owns color-operation snapshots' \
	'^castkms_colorop_snapshot_init\(' \
	src/castkms_plane.c

test -f src/castkms_frame_dispatch.c
test -f src/castkms_frame_dispatch_demand.h
test -f src/castkms_crc.h
test -f src/castkms_capture_job.c
test -f src/castkms_capture_buffer.c
test -f src/castkms_capture_cursor.c
test -f src/castkms_capture_internal.h
test -f src/castkms_connector_uapi.c
test -f src/castkms_grant_core_ioctl_table.inc
test ! -e src/castkms_composer_demand.h

printf '%s\n' 'architecture-dependencies=pass'
