# SPDX-License-Identifier: GPL-2.0-only
# shellcheck shell=bash
#
# Sourced by modules.sh. Scenario modules intentionally share the
# runner's lifecycle state so its single EXIT trap can clean up partial runs.
# Shared state assignments are consumed by that trap after this file returns.
# shellcheck disable=SC2154

append_crc_record()
{
	local destination=$1
	local context=$2
	local line

	if ! IFS= read -r -t 2 -u "$crc_fd" line; then
		printf 'CRC capture stopped during %s\n' "$context" >&2
		return 1
	fi
	if [[ ! $line =~ ^0x[[:xdigit:]]{8}\ 0x[[:xdigit:]]{8}$ ]]; then
		printf 'malformed CRC record during %s: %s\n' "$context" "$line" >&2
		return 1
	fi
	printf '%s\n' "$line" >> "$destination"
}

run_writeback()
{
	local label=$1
	local output=$result_dir/writeback-$label.raw
	local log=$result_dir/writeback-$label.txt
	local status=0

	sudo rm -f "$output"
	sudo timeout --signal=TERM --kill-after=2s 8s \
		modetest -a -M castkms \
		-s "$virtual_connector,$writeback_connector@$crtc_id:1024x768" \
		-P "$plane_id@$crtc_id:1024x768@XR24" \
		-o "$output" </dev/null > "$log" 2>&1 || status=$?
	if test "$status" -ne 0 ||
		! grep -F 'Dumping buffer' "$log" >/dev/null ||
		grep -F 'Poll for writeback error:' "$log" >/dev/null ||
		grep -F 'Atomic Commit failed [1]' "$log" >/dev/null; then
		cat "$log" >&2
		return 1
	fi
	if test "$(stat -c %s "$output")" -ne $((1024 * 768 * 4)); then
		printf 'writeback %s has the wrong size\n' "$label" >&2
		return 1
	fi
	if ! od -An -v -tu1 "$output" | awk '
		{
			for (i = 1; i <= NF; i++) {
				if (!seen) {
					first = $i
					seen = 1
				} else if ($i != first) {
					varied = 1
				}
			}
		}
		END { exit !(seen && varied) }
	'; then
		printf 'writeback %s contains only one byte value\n' "$label" >&2
		return 1
	fi
}

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
	scenario_begin capture
	sudo insmod ./castkms.ko \
		create_default_dev=1 \
		enable_cursor=0 \
		enable_overlay=0 \
		enable_writeback=1 \
		enable_crc=1 \
		enable_plane_pipeline=1 \
		enable_audio=0 \
		enable_cec=1
	cast_loaded=1

	sudo udevadm settle
	ls -l /dev/dri | tee "$result_dir/dev-dri.txt"
	if ! sudo modetest -a -M castkms -c -p -e \
			> "$result_dir/modetest.txt" 2>&1; then
		cat "$result_dir/modetest.txt" >&2
		exit 1
	fi

	virtual_connector=$(awk '
		$1 ~ /^[0-9]+$/ && $4 ~ /^Virtual-/ { print $4; exit }
	' "$result_dir/modetest.txt")
	virtual_connector_id=$(awk '
		$1 ~ /^[0-9]+$/ && $4 ~ /^Virtual-/ { print $1; exit }
	' "$result_dir/modetest.txt")
	writeback_connector=$(awk '
		$1 ~ /^[0-9]+$/ && $4 ~ /^Writeback-/ { print $4; exit }
	' "$result_dir/modetest.txt")
	crtc_id=$(awk '
		$0 == "CRTCs:" { in_crtcs = 1; next }
		in_crtcs && $1 ~ /^[0-9]+$/ { print $1; exit }
	' "$result_dir/modetest.txt")
	plane_id=$(awk '
		$0 == "Planes:" { in_planes = 1; next }
		in_planes && $1 ~ /^[0-9]+$/ { print $1; exit }
	' "$result_dir/modetest.txt")
	test -n "$virtual_connector"
	test -n "$virtual_connector_id"
	test -n "$writeback_connector"
	test -n "$crtc_id"
	test -n "$plane_id"

	castkms_minor=$(sudo find /sys/kernel/debug/dri -maxdepth 1 -type l \
		-lname castkms -printf '%f\n')
	test -n "$castkms_minor"
	castkms_drm=/dev/dri/card$castkms_minor
	test -c "$castkms_drm"

	capture_active_before=$(get_capture_active "$virtual_connector_id")
	test "$capture_active_before" = "0"
	printf '%s\n' 'capture_active_initial=0' | tee -a "$result_dir/summary.txt"

	# Headless guests already have a virtio fbcon, so attaching a sink does not
	# light the castkms CRTC by itself. Watch for the protocol attach and modeset.
	mkfifo "$runtime_dir/connect-mode-gate"
	exec 5<> "$runtime_dir/connect-mode-gate"
	connect_mode_gate_open=1
	sudo stdbuf --output=L --error=L bash -c '
		virtual_connector=$1
		crtc_id=$2
		for _ in $(seq 1 80); do
			if sudo modetest -M castkms -c 2>/dev/null |
				awk -v name="$virtual_connector" \
					"\$4 == name && \$3 == \"connected\" { found = 1 }
					 END { exit !found }"; then
				exec sudo timeout --signal=INT --kill-after=2s 60s \
					stdbuf --output=L --error=L \
					modetest -M castkms \
					-s "$virtual_connector@$crtc_id:1024x768" -v
			fi
			sleep 0.1
		done
		printf "timed out waiting for attached connector\\n" >&2
		exit 1
	' _ "$virtual_connector" "$crtc_id" \
		<&5 > "$result_dir/connect-modeset.txt" 2>&1 &
	connect_modeset_pid=$!

	sudo ./tools/castkms-grant-launch \
		"$castkms_drm" "$virtual_connector_id" -- \
		./tools/castkms-capture-test "$castkms_drm" "$crtc_id" | \
		tee "$result_dir/capture-test.txt"
	grep -Fx 'drm_cap_syncobj=1' "$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'drm_cap_syncobj_timeline=1' \
		"$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_grant_fd=1' "$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_uapi=0.9' "$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_format=XRGB8888:LINEAR' \
		"$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_max_registered_buffers=8' \
		"$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_dmabuf_import=unsupported' \
		"$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_query=pass' "$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_buffer_rejections=pass' \
		"$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_buffer_limit=pass' \
		"$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_buffer_busy=pass' \
		"$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_buffer_implicit=pass' \
		"$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_implicit_fence=pass' \
		"$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_reuse_dependency=pass' \
		"$result_dir/capture-test.txt" >/dev/null
	grep -Eq '^capture_reuse_wait=(observed|not-observed)$' \
		"$result_dir/capture-test.txt"
	grep -Fx 'capture_frame_delivery=pass' \
		"$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_fence_ownership=pass' \
		"$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_damage_validation=pass' \
		"$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_buffer_explicit=pass' \
		"$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_explicit_timeline=pass' \
		"$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_explicit_reuse_dependency=pass' \
		"$result_dir/capture-test.txt" >/dev/null
	grep -Eq '^capture_explicit_reuse_wait=(observed|not-observed)$' \
		"$result_dir/capture-test.txt"
	grep -Fx 'capture_buffer_stop_cleanup=pass' \
		"$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_stop_cancellation=pass' \
		"$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_buffer_registration=pass' \
		"$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_plain_fd_denied=pass' \
		"$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_attach_monitor=pass' \
		"$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_output_edid=pass' \
		"$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_stream_stop=pass' \
		"$result_dir/capture-test.txt" >/dev/null
	grep -Fx 'capture_stream_lifecycle=pass' \
		"$result_dir/capture-test.txt" >/dev/null
	initial_capture_mode_generation=$(sed -n \
		's/^capture_mode_generation=//p' "$result_dir/capture-test.txt")
	[[ $initial_capture_mode_generation =~ ^[0-9]+$ ]]
	printf '%s\n' 'capture_capabilities=pass' | tee -a "$result_dir/summary.txt"
	printf '%s\n' 'capture_stream_lifecycle=pass' | \
		tee -a "$result_dir/summary.txt"
	printf '%s\n' 'capture_buffer_registration=pass' | \
		tee -a "$result_dir/summary.txt"
	printf '%s\n' 'capture_implicit_sync=pass' | \
		tee -a "$result_dir/summary.txt"
	printf '%s\n' 'capture_explicit_sync=pass' | \
		tee -a "$result_dir/summary.txt"
	printf '%s\n' 'capture_frame_delivery=pass' | \
		tee -a "$result_dir/summary.txt"

	sudo ./tools/castkms-grant-launch \
		"$castkms_drm" "$virtual_connector_id" -- \
		./tools/castkms-cec-test "$castkms_drm" | \
		tee "$result_dir/cec-test.txt"
	grep -Eq '^[0-9]+/[0-9]+ tests passed$' "$result_dir/cec-test.txt"
	printf '%s\n' 'cec_transport=pass' | tee -a "$result_dir/summary.txt"

	if test -n "$connect_modeset_pid"; then
		printf '\n' >&5
		exec 5>&-
		connect_mode_gate_open=0
		wait "$connect_modeset_pid" 2>/dev/null || true
		connect_modeset_pid=
	fi

	# Hold a connected monitor so later modeset/writeback jobs see a sink.
	mkfifo "$runtime_dir/attach-gate"
	exec 9<> "$runtime_dir/attach-gate"
	attach_gate_open=1
	sudo stdbuf --output=L --error=L ./tools/castkms-grant-launch \
		"$castkms_drm" "$virtual_connector_id" -- \
		./tools/castkms-attach \
		<&9 > "$result_dir/attach-hold.txt" 2>&1 &
	attach_hold_pid=$!
	attach_ready=0
	for _ in $(seq 1 100); do
		if grep -Fx 'attached=1' "$result_dir/attach-hold.txt" >/dev/null; then
			attach_ready=1
			break
		fi
		if ! kill -0 "$attach_hold_pid" 2>/dev/null; then
			cat "$result_dir/attach-hold.txt" >&2
			exit 1
		fi
		sleep 0.1
	done
	if test "$attach_ready" -ne 1; then
		cat "$result_dir/attach-hold.txt" >&2
		exit 1
	fi
	printf '%s\n' 'capture_attach_hold=pass' | tee -a "$result_dir/summary.txt"

	# Keep stdin open so noninteractive SSH does not end the flip loop immediately.
	page_flip_input_dir=$(mktemp -d)
	mkfifo "$page_flip_input_dir/input"
	exec 3<> "$page_flip_input_dir/input"
	page_flip_gate_open=1
	rm "$page_flip_input_dir/input"
	rmdir "$page_flip_input_dir"

	sudo timeout --signal=INT --kill-after=2s 45s \
		stdbuf --output=L --error=L modetest -M castkms \
		-s "$virtual_connector@$crtc_id:800x600" -v \
		<&3 > "$result_dir/page-flip.txt" 2>&1 &
	page_flip_pid=$!
	page_flip_ready=0
	for _ in $(seq 1 50); do
		if grep -q '^freq:' "$result_dir/page-flip.txt"; then
			page_flip_ready=1
			break
		fi
		if ! kill -0 "$page_flip_pid" 2>/dev/null; then
			break
		fi
		sleep 0.1
	done
	if test "$page_flip_ready" -ne 1; then
		cat "$result_dir/page-flip.txt" >&2
		exit 1
	fi
	sudo ./tools/castkms-grant-launch --capture-only \
		"$castkms_drm" "$virtual_connector_id" -- \
		./tools/castkms-capture-test --mode-generation "$castkms_drm" \
		"$crtc_id" > "$result_dir/capture-test-after-modeset.txt"
	updated_capture_mode_generation=$(sed -n \
		's/^capture_mode_generation=//p' \
		"$result_dir/capture-test-after-modeset.txt")
	[[ $updated_capture_mode_generation =~ ^[0-9]+$ ]]
	test "$updated_capture_mode_generation" -gt \
		"$initial_capture_mode_generation"
	printf '\n' >&3
	exec 3>&-
	page_flip_gate_open=0
	page_flip_status=0
	wait "$page_flip_pid" || page_flip_status=$?
	page_flip_pid=
	if test "$page_flip_status" -ne 0 && test "$page_flip_status" -ne 124; then
		cat "$result_dir/page-flip.txt" >&2
		exit 1
	fi
	printf '%s\n' 'capture_mode_generation=pass' | \
		tee -a "$result_dir/summary.txt"
	if ! sudo drm_info > "$result_dir/drm-info.txt" 2>&1; then
		cat "$result_dir/drm-info.txt" >&2
		exit 1
	fi

	castkms_debugfs=/sys/kernel/debug/dri/castkms
	test "$(sudo sed -n 's/ .*//p' "$castkms_debugfs/name")" = castkms

	# Hold an active atomic modeset while dropping DRM master so independent
	# writeback clients can submit jobs against the same CRTC.
	mkfifo "$runtime_dir/mode-gate"
	exec 8<> "$runtime_dir/mode-gate"
	mode_gate_open=1
	sudo timeout --signal=TERM --kill-after=2s 45s \
		stdbuf --output=L --error=L modetest -M castkms \
		-s "$virtual_connector@$crtc_id:1024x768" -v \
		<&8 > "$result_dir/mode-holder.txt" 2>&1 &
	mode_holder_pid=$!
	mode_active=0
	for _ in $(seq 1 50); do
		if ! kill -0 "$mode_holder_pid" 2>/dev/null; then
			cat "$result_dir/mode-holder.txt" >&2
			exit 1
		fi
		if sudo modetest -a -M castkms -p \
				> "$result_dir/mode-state.txt" 2>&1 &&
			awk -v crtc="$crtc_id" '
				$1 == crtc && $4 == "(1024x768)" { found = 1 }
				END { exit !found }
			' "$result_dir/mode-state.txt"; then
			mode_active=1
			break
		fi
		sleep 0.1
	done
	if test "$mode_active" -ne 1 ||
		grep -F 'Atomic Commit failed' "$result_dir/mode-holder.txt" \
			>/dev/null; then
		cat "$result_dir/mode-holder.txt" >&2
		exit 1
	fi

	printf 'auto\n' | sudo tee "$castkms_debugfs/crtc-0/crc/control" >/dev/null
	coproc CRC_CAPTURE {
		exec sudo timeout --signal=TERM --kill-after=1s 30s \
			cat "$castkms_debugfs/crtc-0/crc/data" \
			2> "$result_dir/crc-reader.txt"
	}
	crc_pid=$CRC_CAPTURE_PID
	crc_fd=${CRC_CAPTURE[0]}
	: > "$result_dir/crc.txt"
	: > "$result_dir/crc-writeback.txt"
	for _ in 1 2 3; do
		append_crc_record "$result_dir/crc.txt" baseline
	done
	printf '%s\n' 'composer_crc=pass' | tee -a "$result_dir/summary.txt"

	capture_overlap_status=0
	sudo stdbuf --output=L --error=L ./tools/castkms-grant-launch \
		--capture-only "$castkms_drm" "$virtual_connector_id" -- \
		./tools/castkms-capture-test --deliver-one \
		"$castkms_drm" "$crtc_id" \
		> "$result_dir/capture-writeback-overlap.txt" 2>&1 || \
		capture_overlap_status=$?
	if test "$capture_overlap_status" -ne 0; then
		cat "$result_dir/capture-writeback-overlap.txt" >&2
		exit 1
	fi
	grep -Fx 'capture_overlap_queued=1' \
		"$result_dir/capture-writeback-overlap.txt" >/dev/null
	grep -Fx 'capture_writeback_overlap=pass' \
		"$result_dir/capture-writeback-overlap.txt" >/dev/null
	printf '%s\n' 'composer_capture_writeback_overlap=pass' | \
		tee -a "$result_dir/summary.txt"

	printf '\n' >&8
	exec 8>&-
	mode_gate_open=0
	mode_holder_status=0
	wait "$mode_holder_pid" || mode_holder_status=$?
	mode_holder_pid=
	if test "$mode_holder_status" -ne 0 &&
		test "$mode_holder_status" -ne 124; then
		cat "$result_dir/mode-holder.txt" >&2
		exit 1
	fi

	run_writeback 1
	run_writeback 2
	cmp -s "$result_dir/writeback-1.raw" "$result_dir/writeback-2.raw"
	sha256sum "$result_dir/writeback-1.raw" "$result_dir/writeback-2.raw" \
		> "$result_dir/writeback-sha256.txt"

	# Writeback runs as DRM master, so the earlier holder had to exit. Light the
	# CRTC again so CRC records continue after those jobs.
	rm -f "$runtime_dir/mode-gate"
	mkfifo "$runtime_dir/mode-gate"
	exec 8<> "$runtime_dir/mode-gate"
	mode_gate_open=1
	sudo timeout --signal=TERM --kill-after=2s 20s \
		stdbuf --output=L --error=L modetest -M castkms \
		-s "$virtual_connector@$crtc_id:1024x768" -v \
		<&8 > "$result_dir/mode-holder-crc.txt" 2>&1 &
	mode_holder_pid=$!
	mode_active=0
	for _ in $(seq 1 50); do
		if ! kill -0 "$mode_holder_pid" 2>/dev/null; then
			cat "$result_dir/mode-holder-crc.txt" >&2
			exit 1
		fi
		if sudo modetest -a -M castkms -p \
				> "$result_dir/mode-state-crc.txt" 2>&1 &&
			awk -v crtc="$crtc_id" '
				$1 == crtc && $4 == "(1024x768)" { found = 1 }
				END { exit !found }
			' "$result_dir/mode-state-crc.txt"; then
			mode_active=1
			break
		fi
		sleep 0.1
	done
	if test "$mode_active" -ne 1; then
		cat "$result_dir/mode-holder-crc.txt" >&2
		exit 1
	fi

	# Discard records already queued during the second job. Reaching an
	# inter-frame gap before requiring new records prevents buffered output from
	# hiding a composer that stopped during writeback cleanup.
	drained=0
	while test "$drained" -lt 1024; do
		if ! IFS= read -r -t 0.001 -u "$crc_fd" line; then
			break
		fi
		if [[ ! $line =~ ^0x[[:xdigit:]]{8}\ 0x[[:xdigit:]]{8}$ ]]; then
			printf 'malformed queued CRC record: %s\n' "$line" >&2
			exit 1
		fi
		printf '%s\n' "$line" >> "$result_dir/crc-writeback.txt"
		drained=$((drained + 1))
	done
	test "$drained" -lt 1024
	for _ in 1 2 3; do
		append_crc_record "$result_dir/crc-writeback.txt" \
			'post-writeback cleanup'
	done
	printf '%s\n' 'composer_writeback_overlap=pass' | \
		tee -a "$result_dir/summary.txt"

	exec {crc_fd}<&-
	crc_fd=
	wait "$crc_pid" 2>/dev/null || true
	crc_pid=

	if test "$mode_gate_open" -eq 1; then
		printf '\n' >&8
		exec 8>&-
		mode_gate_open=0
	fi
	if test -n "$mode_holder_pid"; then
		wait "$mode_holder_pid" 2>/dev/null || true
		mode_holder_pid=
	fi

	printf 'x' >&9
	exec 9>&-
	attach_gate_open=0
	attach_hold_status=0
	wait "$attach_hold_pid" || attach_hold_status=$?
	attach_hold_pid=
	if test "$attach_hold_status" -ne 0; then
		cat "$result_dir/attach-hold.txt" >&2
		exit 1
	fi

	sudo rmmod castkms
	cast_loaded=0
	if lsmod | grep -Eq '^(vkms|castkms)\b'; then
		printf '%s\n' 'module cleanup check failed' >&2
		exit 1
	fi
	test ! -e /sys/kernel/config/vkms
	test ! -e /sys/kernel/config/castkms

	printf '%s\n' 'cleanup=pass' | tee -a "$result_dir/summary.txt"
	scenario_end capture
}
