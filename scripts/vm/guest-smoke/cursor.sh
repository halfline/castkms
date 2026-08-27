# SPDX-License-Identifier: GPL-2.0-only
# shellcheck shell=bash

run_cursor_scenario()
{
	scenario_begin cursor
	sudo insmod ./castkms.ko \
		create_default_dev=1 \
		enable_cursor=1 \
		enable_writeback=0 \
		enable_overlay=0 \
		enable_audio=0
	cast_loaded=1
	sudo udevadm settle

	castkms_minor=$(sudo find /sys/kernel/debug/dri -maxdepth 1 -type l \
		-lname castkms -printf '%f\n')
	test -n "$castkms_minor"
	castkms_drm=/dev/dri/card$castkms_minor
	test -c "$castkms_drm"

	if ! sudo modetest -M castkms -c -p \
			> "$result_dir/cursor-modetest.txt" 2>&1; then
		cat "$result_dir/cursor-modetest.txt" >&2
		exit 1
	fi
	crtc_id=$(awk '
		$0 == "CRTCs:" { in_crtcs = 1; next }
		in_crtcs && $1 ~ /^[0-9]+$/ { print $1; exit }
	' "$result_dir/cursor-modetest.txt")
	virtual_connector_id=$(awk '
		$1 ~ /^[0-9]+$/ && $4 ~ /^Virtual-/ { print $1; exit }
	' "$result_dir/cursor-modetest.txt")
	test -n "$crtc_id"
	test -n "$virtual_connector_id"

	sudo ./tools/castkms-grant-launch \
		"$castkms_drm" "$virtual_connector_id" -- \
		./tools/castkms-capture-test --cursor "$castkms_drm" "$crtc_id" | \
		tee "$result_dir/cursor-test.txt"
	grep -Fx 'cursor_metadata=pass' "$result_dir/cursor-test.txt" >/dev/null
	grep -Fx 'cursor_bitmap=pass' "$result_dir/cursor-test.txt" >/dev/null
	grep -Fx 'cursor_no_change=pass' "$result_dir/cursor-test.txt" >/dev/null
	grep -Fx 'cursor_stream_image_state=pass' \
		"$result_dir/cursor-test.txt" >/dev/null
	grep -Fx 'cursor_move=pass' "$result_dir/cursor-test.txt" >/dev/null
	grep -Fx 'cursor_image_changed=pass' "$result_dir/cursor-test.txt" >/dev/null
	grep -Fx 'cursor_clear=pass' "$result_dir/cursor-test.txt" >/dev/null
	grep -Fx 'cursor_hidden_bitmap=pass' "$result_dir/cursor-test.txt" >/dev/null
	grep -Fx 'cursor_test=pass' "$result_dir/cursor-test.txt" >/dev/null
	printf '%s\n' 'capture_cursor_metadata=pass' | tee -a "$result_dir/summary.txt"

	sudo rmmod castkms
	cast_loaded=0
	scenario_end cursor
}
