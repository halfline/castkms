# SPDX-License-Identifier: GPL-2.0-only
# shellcheck shell=bash

get_capture_active()
{
	sudo modetest -M castkms -c 2>/dev/null | awk -v cid="$1" '
		$1 == cid { found_connector = 1; next }
		found_connector && /^[0-9]/ { exit }
		found_connector && /capture_active:/ { in_prop = 1; next }
		in_prop && $1 == "value:" { print $2; exit }
	'
}

run_capture_scenario()
{
	capture_active_before=$(get_capture_active "$virtual_connector_id")
	test "$capture_active_before" = "0"
	printf '%s\n' 'capture_active_initial=0' | tee -a "$result_dir/summary.txt"
}
