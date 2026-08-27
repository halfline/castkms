#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo_dir"

layer_manifest=scripts/architecture-layers.txt
declare -A source_layers=()
manifest_files=()

while read -r layer path extra; do
	case "$layer" in
	''|'#'*) continue ;;
	esac
	if test -n "${extra:-}"; then
		printf 'invalid architecture manifest entry: %s %s %s\n' \
			"$layer" "$path" "$extra" >&2
		exit 1
	fi
	case "$layer" in
	assembly|audio-core|authority-core|capture-core|capture-uapi|cec-core|cec-uapi|config|drm-runtime|grant-uapi|render-core) ;;
	*)
		printf 'unknown architecture layer: %s\n' "$layer" >&2
		exit 1
		;;
	esac
	if test ! -f "$path"; then
		printf 'stale architecture manifest path: %s\n' "$path" >&2
		exit 1
	fi
	if test -n "${source_layers[$path]:-}"; then
		printf 'duplicate architecture manifest path: %s\n' "$path" >&2
		exit 1
	fi
	source_layers[$path]=$layer
	manifest_files+=("$path")
done < "$layer_manifest"

mapfile -t current_sources < <(
	git ls-files --cached --others --exclude-standard -- src | \
	while IFS= read -r path; do
		case "$path" in
		src/tests/*) ;;
		src/*.c|src/*.h)
			test -f "$path" && printf '%s\n' "$path"
			;;
		esac
	done | sort
)

if ! diff -u \
	<(printf '%s\n' "${manifest_files[@]}" | sort) \
	<(printf '%s\n' "${current_sources[@]}"); then
	printf '%s\n' \
		'architecture manifest does not classify every production source exactly once' >&2
	exit 1
fi

layer_files()
{
	local wanted_layer=$1
	local path

	for path in "${manifest_files[@]}"; do
		if test "${source_layers[$path]}" = "$wanted_layer"; then
			printf '%s\n' "$path"
		fi
	done
}

check_layer_dependencies()
{
	local dependency_header
	local dependency_layer
	local dependency_path
	local path
	local source_layer

	for path in "${manifest_files[@]}"; do
		source_layer=${source_layers[$path]}
		while IFS= read -r dependency_header; do
			dependency_path=src/$dependency_header
			dependency_layer=${source_layers[$dependency_path]:-}
			case "$source_layer:$dependency_layer" in
			audio-core:grant-uapi|audio-core:capture-uapi|audio-core:cec-uapi|\
			authority-core:grant-uapi|authority-core:capture-uapi|authority-core:cec-uapi|\
			capture-core:grant-uapi|capture-core:capture-uapi|capture-core:cec-uapi|\
			cec-core:grant-uapi|cec-core:capture-uapi|cec-core:cec-uapi|\
			render-core:grant-uapi|render-core:capture-core|render-core:capture-uapi|render-core:cec-uapi)
				printf 'architecture dependency violation: %s (%s) imports %s (%s)\n' \
					"$path" "$source_layer" "$dependency_path" \
					"$dependency_layer" >&2
				exit 1
				;;
			esac
		done < <(
			sed -n \
				's/^[[:space:]]*#include[[:space:]]*"\(castkms_[^"]*\.h\)".*/\1/p' \
				"$path"
		)
	done
}

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

check_layer_dependencies

reject 'output runtime header imports a complete subsystem API' \
	'#include "castkms_(capture|composer|frame)\.h"' \
	src/castkms_output.h

reject 'core device exposes grant-fd registry state' \
	'grant_registry|next_grant_id|struct xarray grants' \
	src/castkms_device.h

reject 'CEC core depends on the CastKMS device layout' \
	'castkms_device\.h|struct castkms_device' \
	src/castkms_cec_core.c src/castkms_cec_core.h

mapfile -t transport_core_files < <(
	layer_files audio-core
	layer_files authority-core
	layer_files capture-core
	layer_files cec-core
)
reject 'transport-neutral core imports grant, event, or public UAPI state' \
	'castkms_grant|castkms_uapi_device|drm_pending_event|castkms_drm\.h' \
	"${transport_core_files[@]}"

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
