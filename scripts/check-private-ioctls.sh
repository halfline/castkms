#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "$script_dir/.." && pwd)
work_dir=$(mktemp -d)

# shellcheck disable=SC2329 # Invoked indirectly by the EXIT trap.
cleanup()
{
	rm -rf -- "$work_dir"
}
trap cleanup EXIT

sed -n \
	's/^#define[[:space:]]\+DRM_IOCTL_\(CASTKMS_[A-Z0-9_]*\).*/\1/p' \
	"$repo_dir/include/uapi/drm/castkms_drm.h" | sort \
	> "$work_dir/uapi"
sed -n \
	's/^CASTKMS_PRIVATE_IOCTL(\(CASTKMS_[A-Z0-9_]*\),.*/\1/p' \
	"$repo_dir/src/castkms_ioctl_table.inc" | sort > "$work_dir/metadata"
sed -n \
	's/^CASTKMS_GRANT_CORE_IOCTL(\([A-Z0-9_]*\)).*/\1/p' \
	"$repo_dir/src/castkms_grant_core_ioctl_table.inc" | sort \
	> "$work_dir/core-metadata"

if test -s "$work_dir/uapi" &&
	test -s "$work_dir/metadata" &&
	test ! -s <(uniq -d "$work_dir/uapi") &&
	test ! -s <(uniq -d "$work_dir/metadata") &&
	diff -u "$work_dir/uapi" "$work_dir/metadata"; then
	printf 'private-ioctl-metadata=pass (%s commands)\n' \
		"$(wc -l < "$work_dir/uapi")"
else
	printf '%s\n' 'private ioctl metadata does not exhaust the public commands' >&2
	exit 1
fi

if test -s "$work_dir/core-metadata" &&
	test ! -s <(uniq -d "$work_dir/core-metadata"); then
	printf 'grant-core-ioctl-metadata=pass (%s commands)\n' \
		"$(wc -l < "$work_dir/core-metadata")"
else
	printf '%s\n' 'grant core ioctl metadata is empty or contains duplicates' >&2
	exit 1
fi
