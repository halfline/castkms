#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Load the default castkms device if needed and hold ATTACH_MONITOR. Intended
# to run as a systemd service so the sink stays plugged after SSH exits.

set -euo pipefail

repo_dir=${1:-$HOME/castkms}
ko=${CASTKMS_KO:-$repo_dir/castkms.ko}
tool=${CASTKMS_ATTACH:-$repo_dir/tools/castkms-attach}
grant_tool=${CASTKMS_GRANT_LAUNCH:-$repo_dir/tools/castkms-grant-launch}
dependency_helper=${CASTKMS_MODULE_DEPENDENCIES:-$repo_dir/scripts/vm/module-dependencies.sh}

PATH=/usr/sbin:/usr/bin
export PATH

if test ! -r "$dependency_helper"; then
	printf '%s\n' "missing module dependency helper: $dependency_helper" >&2
	exit 1
fi
. "$dependency_helper"

if test ! -x "$tool"; then
	printf '%s\n' "missing capture tool: $tool" >&2
	exit 1
fi
if test ! -x "$grant_tool"; then
	printf '%s\n' "missing grant issuer: $grant_tool" >&2
	exit 1
fi

if ! mountpoint -q /sys/kernel/debug; then
	mount -t debugfs none /sys/kernel/debug
fi

if ! lsmod | grep -Eq '^castkms\b'; then
	if test ! -f "$ko"; then
		printf '%s\n' "missing module: $ko" >&2
		exit 1
	fi
	load_module_dependencies "$ko"
	insmod "$ko" \
		create_default_dev=1 \
		enable_cursor=0 \
		enable_overlay=0 \
		enable_writeback=0 \
		enable_plane_pipeline=0
	udevadm settle
fi

castkms_minor=$(find /sys/kernel/debug/dri -maxdepth 1 -type l \
	-lname castkms -printf '%f\n')
if test -z "$castkms_minor"; then
	printf '%s\n' 'no castkms DRM card found' >&2
	exit 1
fi
castkms_drm=/dev/dri/card$castkms_minor
if test ! -c "$castkms_drm"; then
	printf '%s\n' "missing DRM node: $castkms_drm" >&2
	exit 1
fi

connector_id=$(modetest -a -M castkms -c | awk '
	$1 ~ /^[0-9]+$/ && $4 ~ /^Virtual-/ { print $1; exit }
')
if test -z "$connector_id"; then
	printf '%s\n' 'no CastKMS virtual connector found' >&2
	exit 1
fi

fifo_dir=$(mktemp -d)
fifo=$fifo_dir/hold
mkfifo "$fifo"
exec 3<>"$fifo"
rm -f "$fifo"
rmdir "$fifo_dir"
exec "$grant_tool" "$castkms_drm" "$connector_id" -- \
	"$tool" <&3
