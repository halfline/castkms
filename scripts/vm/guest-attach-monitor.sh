#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Load the default castkms device if needed and hold ATTACH_MONITOR. Intended
# to run as a systemd service so the sink stays plugged after SSH exits.

set -euo pipefail

repo_dir=${1:-$HOME/castkms}
ko=${CASTKMS_KO:-$repo_dir/castkms.ko}
tool=${CASTKMS_CAPTURE_TEST:-$repo_dir/tools/castkms-capture-test}

PATH=/usr/sbin:/usr/bin
export PATH

if test ! -x "$tool"; then
	printf '%s\n' "missing capture tool: $tool" >&2
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

crtc_id=$(modetest -a -M castkms -c -p | awk '
	$0 == "CRTCs:" { in_crtcs = 1; next }
	in_crtcs && $1 ~ /^[0-9]+$/ { print $1; exit }
')
if test -z "$crtc_id"; then
	printf '%s\n' 'no castkms CRTC found' >&2
	exit 1
fi

fifo=$(mktemp -u)
mkfifo "$fifo"
exec 3<>"$fifo"
rm -f "$fifo"
exec "$tool" --attach "$castkms_drm" "$crtc_id" <&3
