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

rg -q '^castkms_colorop_snapshot_init\(' src/castkms_colorop.c
reject 'plane layer owns color-operation snapshots' \
	'^castkms_colorop_snapshot_init\(' \
	src/castkms_plane.c

printf '%s\n' 'architecture-dependencies=pass'
