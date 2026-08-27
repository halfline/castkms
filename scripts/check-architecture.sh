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

reject 'pixel composer coordinates mutable frame consumers' \
	'castkms_capture|castkms_capture_owner|castkms_crtc|castkms_frame_dispatch|drm_writeback|drm_crtc_add_crc' \
	src/castkms_composer.c src/castkms_composer.h

rg -q '^castkms_colorop_snapshot_init\(' src/castkms_colorop.c
reject 'plane layer owns color-operation snapshots' \
	'^castkms_colorop_snapshot_init\(' \
	src/castkms_plane.c

test -f src/castkms_frame_dispatch.c
test -f src/castkms_frame_dispatch_demand.h
test -f src/castkms_crc.h
test ! -e src/castkms_composer_demand.h

printf '%s\n' 'architecture-dependencies=pass'
