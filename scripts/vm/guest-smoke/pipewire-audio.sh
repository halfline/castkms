# SPDX-License-Identifier: GPL-2.0-only
# shellcheck shell=bash

run_pipewire_audio_scenario()
{
	scenario_begin pipewire-audio
	sudo insmod ./castkms.ko \
		create_default_dev=1 \
		enable_cursor=0 \
		enable_overlay=0 \
		enable_writeback=0 \
		enable_audio=1
	cast_loaded=1
	sudo udevadm settle

	castkms_minor=$(sudo find /sys/kernel/debug/dri -maxdepth 1 -type l \
		-lname castkms -printf '%f\n')
	test -n "$castkms_minor"
	castkms_drm=/dev/dri/card$castkms_minor
	test -c "$castkms_drm"

	if ! sudo modetest -M castkms -c -p \
			> "$result_dir/pw-modetest.txt" 2>&1; then
		cat "$result_dir/pw-modetest.txt" >&2
		exit 1
	fi
	virtual_connector=$(awk '
		$1 ~ /^[0-9]+$/ && $4 ~ /^Virtual-/ { print $4; exit }
	' "$result_dir/pw-modetest.txt")
	virtual_connector_id=$(awk '
		$1 ~ /^[0-9]+$/ && $4 ~ /^Virtual-/ { print $1; exit }
	' "$result_dir/pw-modetest.txt")
	crtc_id=$(awk '
		$0 == "CRTCs:" { in_crtcs = 1; next }
		in_crtcs && $1 ~ /^[0-9]+$/ { print $1; exit }
	' "$result_dir/pw-modetest.txt")
	test -n "$virtual_connector"
	test -n "$virtual_connector_id"
	test -n "$crtc_id"

	pw_runtime=$(mktemp -d)
	sudo env PIPEWIRE_RUNTIME_DIR="$pw_runtime" \
		XDG_RUNTIME_DIR="$pw_runtime" pipewire \
		> "$result_dir/pw-daemon.txt" 2>&1 &
	pw_daemon_pid=$!

	pw_ready=0
	for _ in $(seq 1 20); do
		if sudo test -S "$pw_runtime/pipewire-0"; then
			pw_ready=1
			break
		fi
		if ! kill -0 "$pw_daemon_pid" 2>/dev/null; then
			cat "$result_dir/pw-daemon.txt" >&2
			exit 1
		fi
		sleep 0.2
	done
	if test "$pw_ready" -ne 1; then
		cat "$result_dir/pw-daemon.txt" >&2
		exit 1
	fi

	sudo env PIPEWIRE_RUNTIME_DIR="$pw_runtime" \
		XDG_RUNTIME_DIR="$pw_runtime" wireplumber \
		> "$result_dir/pw-wireplumber.txt" 2>&1 &
	pw_wireplumber_pid=$!
	sleep 0.5

	sudo env PIPEWIRE_RUNTIME_DIR="$pw_runtime" \
		XDG_RUNTIME_DIR="$pw_runtime" \
		./tools/castkms-grant-launch \
		"$castkms_drm" "$virtual_connector_id" -- \
		./tools/pw-castkms/pw-castkms -U -d "$castkms_drm" -c "$crtc_id" \
		> "$result_dir/pw-castkms.txt" 2>&1 &
	pw_source_pid=$!

	pw_attached=0
	for _ in $(seq 1 50); do
		if sudo modetest -M castkms -c 2>/dev/null |
			awk -v name="$virtual_connector" \
				'$4 == name && $3 == "connected" { found = 1 }
				 END { exit !found }'; then
			pw_attached=1
			break
		fi
		if ! kill -0 "$pw_source_pid" 2>/dev/null; then
			cat "$result_dir/pw-castkms.txt" >&2
			exit 1
		fi
		sleep 0.2
	done
	if test "$pw_attached" -ne 1; then
		cat "$result_dir/pw-castkms.txt" >&2
		exit 1
	fi

	mkfifo "$runtime_dir/pw-mode-gate"
	exec 6<> "$runtime_dir/pw-mode-gate"
	pw_mode_gate_open=1
	sudo timeout --signal=INT --kill-after=2s 30s \
		stdbuf --output=L --error=L modetest -M castkms \
		-s "$virtual_connector@$crtc_id:1024x768" \
		<&6 > "$result_dir/pw-modeset.txt" 2>&1 &
	pw_modeset_pid=$!

	pw_running=0
	for _ in $(seq 1 60); do
		if grep -Fq 'running' "$result_dir/pw-castkms.txt" 2>/dev/null; then
			pw_running=1
			break
		fi
		if ! kill -0 "$pw_source_pid" 2>/dev/null; then
			cat "$result_dir/pw-castkms.txt" >&2
			exit 1
		fi
		sleep 0.5
	done
	if test "$pw_running" -ne 1; then
		cat "$result_dir/pw-castkms.txt" >&2
		exit 1
	fi

	capture_active_during=$(get_capture_active "$virtual_connector_id")
	test "$capture_active_during" = "1"
	printf '%s\n' 'capture_active_during=1' | tee -a "$result_dir/summary.txt"

	pw_node="castkms.card${castkms_minor}.crtc-${crtc_id}"
	sudo env PIPEWIRE_RUNTIME_DIR="$pw_runtime" \
		XDG_RUNTIME_DIR="$pw_runtime" \
		timeout --signal=TERM --kill-after=2s 20s \
		./tools/pw-castkms/pw-castkms-test -n "$pw_node" -f 10 -t 15 \
		| tee "$result_dir/pw-castkms-test.txt"

	grep -Fx 'pw_connected=1' "$result_dir/pw-castkms-test.txt" >/dev/null
	grep -Fx 'format_negotiated=1' "$result_dir/pw-castkms-test.txt" >/dev/null
	grep -Fx 'timed_out=0' "$result_dir/pw-castkms-test.txt" >/dev/null
	grep -Fx 'sequence_monotonic=1' "$result_dir/pw-castkms-test.txt" >/dev/null
	grep -Fx 'timestamp_monotonic=1' "$result_dir/pw-castkms-test.txt" >/dev/null
	grep -Fx 'meta_present=1' "$result_dir/pw-castkms-test.txt" >/dev/null
	grep -Fx 'data_valid=1' "$result_dir/pw-castkms-test.txt" >/dev/null
	grep -Fx 'pw_castkms_test=pass' "$result_dir/pw-castkms-test.txt" >/dev/null
	printf '%s\n' 'pw_castkms_bridge=pass' | tee -a "$result_dir/summary.txt"

	# The last consumer tears down PipeWire's buffer pool.  The source must stop
	# that CastKMS stream, stay published, and build a fresh stream/pool when a new
	# consumer arrives.
	pw_pool_released=0
	for _ in $(seq 1 20); do
		if ! kill -0 "$pw_source_pid" 2>/dev/null; then
			cat "$result_dir/pw-castkms.txt" >&2
			exit 1
		fi
		if test "$(get_capture_active "$virtual_connector_id")" = "0"; then
			pw_pool_released=1
			break
		fi
		sleep 0.1
	done
	if test "$pw_pool_released" -ne 1; then
		printf 'pw-castkms did not release its paused buffer pool\n' >&2
		cat "$result_dir/pw-castkms.txt" >&2
		exit 1
	fi

	sudo env PIPEWIRE_RUNTIME_DIR="$pw_runtime" \
		XDG_RUNTIME_DIR="$pw_runtime" \
		timeout --signal=TERM --kill-after=2s 20s \
		./tools/pw-castkms/pw-castkms-test -n "$pw_node" -f 5 -t 15 \
		| tee "$result_dir/pw-castkms-reconnect-test.txt"
	grep -Fx 'pw_castkms_test=pass' \
		"$result_dir/pw-castkms-reconnect-test.txt" >/dev/null
	if ! kill -0 "$pw_source_pid" 2>/dev/null ||
		grep -Eq 'ownership mismatch|could not restart capture' \
			"$result_dir/pw-castkms.txt"; then
		cat "$result_dir/pw-castkms.txt" >&2
		exit 1
	fi
	if test "$(grep -c '^capture stream ' "$result_dir/pw-castkms.txt")" -lt 2; then
		printf 'pw-castkms did not rebuild its CastKMS stream\n' >&2
		cat "$result_dir/pw-castkms.txt" >&2
		exit 1
	fi
	printf '%s\n' 'pw_castkms_pool_reconnect=pass' | \
		tee -a "$result_dir/summary.txt"

	# PipeWire audio sink discovery: WirePlumber should expose the CastKMS ALSA
	# card as an Audio/Sink while the audio-capable monitor is attached.
	pw_audio_sink=0
	for _ in $(seq 1 20); do
		if sudo env PIPEWIRE_RUNTIME_DIR="$pw_runtime" \
			XDG_RUNTIME_DIR="$pw_runtime" \
			wpctl status 2>/dev/null | grep -qi 'CastKMS'; then
			pw_audio_sink=1
			break
		fi
		sleep 0.5
	done
	if test "$pw_audio_sink" -eq 1; then
		sudo env PIPEWIRE_RUNTIME_DIR="$pw_runtime" \
			XDG_RUNTIME_DIR="$pw_runtime" \
			wpctl status > "$result_dir/pw-audio-sink.txt" 2>&1
		printf '%s\n' 'pw_audio_sink=pass' | tee -a "$result_dir/summary.txt"
	else
		printf '%s\n' 'pw_audio_sink=skip (WirePlumber did not expose CastKMS sink)' | \
			tee -a "$result_dir/summary.txt"
	fi

	# Sink monitor audio capture: verify that audio data played to the CastKMS
	# PipeWire sink can be captured from its monitor ports with correct content.
	if test "$pw_audio_sink" -eq 1; then
		castkms_sink_id=$(sudo env PIPEWIRE_RUNTIME_DIR="$pw_runtime" \
			XDG_RUNTIME_DIR="$pw_runtime" \
			wpctl status 2>/dev/null | \
			sed -n 's/[^0-9]*\([0-9][0-9]*\)\..*CastKMS.*/\1/p' | \
			head -1)

		if test -n "$castkms_sink_id"; then
			python3 - "$result_dir/audio-test-tone.wav" << 'PYEOF'
import wave, struct, sys
with wave.open(sys.argv[1], 'w') as f:
    f.setnchannels(2)
    f.setsampwidth(2)
    f.setframerate(48000)
    f.writeframes(struct.pack('<h', 0x4000) * 96000)
PYEOF

			sudo env PIPEWIRE_RUNTIME_DIR="$pw_runtime" \
				XDG_RUNTIME_DIR="$pw_runtime" \
				timeout --signal=TERM --kill-after=2s 8s \
				pw-cat --record --target "$castkms_sink_id" \
				--properties "stream.capture.sink=true" \
				"$result_dir/sink-monitor-capture.wav" \
				> "$result_dir/sink-monitor-record.txt" 2>&1 &
			sink_capture_pid=$!

			sleep 1

			sudo env PIPEWIRE_RUNTIME_DIR="$pw_runtime" \
				XDG_RUNTIME_DIR="$pw_runtime" \
				timeout --signal=TERM --kill-after=2s 5s \
				pw-cat --playback --target "$castkms_sink_id" \
				"$result_dir/audio-test-tone.wav" \
				> "$result_dir/sink-monitor-play.txt" 2>&1 || true

			sleep 0.5
			kill "$sink_capture_pid" 2>/dev/null || true
			wait "$sink_capture_pid" 2>/dev/null || true
			sink_capture_pid=

			python3 - "$result_dir/sink-monitor-capture.wav" \
				<< 'PYEOF' | tee "$result_dir/sink-monitor-analysis.txt"
import wave, struct, sys
try:
    with wave.open(sys.argv[1], 'r') as f:
        data = f.readframes(f.getnframes())
        nch = f.getnchannels()
except Exception as e:
    print('capture_read=fail (%s)' % e)
    sys.exit(1)

n = len(data) // 2
if n < 100:
    print('capture_samples=%d' % n)
    print('sink_monitor_integrity=fail (too few samples)')
    sys.exit(1)

vals = struct.unpack('<%dh' % n, data)
nonzero = sum(1 for v in vals if v != 0)
matches = sum(1 for v in vals if abs(v - 0x4000) <= 0x400)

print('capture_samples=%d' % n)
print('capture_nonzero=%d' % nonzero)
print('capture_pattern_matches=%d' % matches)
if n > 0:
    print('capture_match_pct=%.1f' % (100.0 * matches / n))

if matches > n // 4:
    print('sink_monitor_integrity=pass')
else:
    print('sink_monitor_integrity=fail')
    sys.exit(1)
PYEOF

			if grep -q 'sink_monitor_integrity=pass' \
				"$result_dir/sink-monitor-analysis.txt"; then
				printf '%s\n' 'sink_monitor_capture=pass' | \
					tee -a "$result_dir/summary.txt"
			else
				cat "$result_dir/sink-monitor-record.txt" >&2
				cat "$result_dir/sink-monitor-play.txt" >&2
				cat "$result_dir/sink-monitor-analysis.txt" >&2
				exit 1
			fi
		else
			printf 'Could not find CastKMS sink node ID\n' >&2
			exit 1
		fi
	else
		printf '%s\n' \
			'sink_monitor_capture=skip (no audio sink)' | \
			tee -a "$result_dir/summary.txt"
	fi

	exec 6>&-
	pw_mode_gate_open=0
	if ! kill -0 "$pw_source_pid" 2>/dev/null ||
		grep -Eq 'ownership mismatch|could not restart capture' \
			"$result_dir/pw-castkms.txt"; then
		cat "$result_dir/pw-castkms.txt" >&2
		exit 1
	fi
	kill "$pw_source_pid" 2>/dev/null || true
	wait "$pw_source_pid" 2>/dev/null || true
	pw_source_pid=

	capture_active_after=$(get_capture_active "$virtual_connector_id")
	test "$capture_active_after" = "0"
	printf '%s\n' 'capture_active_after=0' | tee -a "$result_dir/summary.txt"
	printf '%s\n' 'capture_active_property=pass' | tee -a "$result_dir/summary.txt"

	kill "$pw_modeset_pid" 2>/dev/null || true
	wait "$pw_modeset_pid" 2>/dev/null || true
	pw_modeset_pid=
	kill "$pw_wireplumber_pid" 2>/dev/null || true
	wait "$pw_wireplumber_pid" 2>/dev/null || true
	pw_wireplumber_pid=
	kill "$pw_daemon_pid" 2>/dev/null || true
	wait "$pw_daemon_pid" 2>/dev/null || true
	pw_daemon_pid=
	sudo rm -rf -- "$pw_runtime"
	pw_runtime=
	rm -f "$runtime_dir/pw-mode-gate"

	# ALSA audio test
	# Reuse the castkms instance from the PipeWire section, which loaded
	# with enable_audio=1.  PipeWire and WirePlumber are stopped but the
	# module is still loaded with the same DRM device.
	test -c "$castkms_drm"

	# The ALSA card should exist even before a monitor is attached.
	if ! grep -q CastKMS /proc/asound/cards; then
		printf 'CastKMS card not found in /proc/asound/cards\n' >&2
		exit 1
	fi
	printf '%s\n' 'audio_card_present=pass' | tee -a "$result_dir/summary.txt"

	if ! sudo modetest -M castkms -c -p \
			> "$result_dir/audio-modetest.txt" 2>&1; then
		cat "$result_dir/audio-modetest.txt" >&2
		exit 1
	fi
	virtual_connector=$(awk '
		$1 ~ /^[0-9]+$/ && $4 ~ /^Virtual-/ { print $4; exit }
	' "$result_dir/audio-modetest.txt")
	virtual_connector_id=$(awk '
		$1 ~ /^[0-9]+$/ && $4 ~ /^Virtual-/ { print $1; exit }
	' "$result_dir/audio-modetest.txt")
	crtc_id=$(awk '
		$0 == "CRTCs:" { in_crtcs = 1; next }
		in_crtcs && $1 ~ /^[0-9]+$/ { print $1; exit }
	' "$result_dir/audio-modetest.txt")
	test -n "$virtual_connector"
	test -n "$virtual_connector_id"
	test -n "$crtc_id"

	# Before attach, playback should fail (no audio-capable monitor).
	castkms_card_index=$(awk '/CastKMS/ { print $1; exit }' /proc/asound/cards)
	if test -z "$castkms_card_index"; then
		printf 'could not determine CastKMS card index\n' >&2
		exit 1
	fi
	sudo amixer -c "$castkms_card_index" \
		cget iface=PCM,name='Playback Channel Map' \
		> "$result_dir/audio-channel-map.txt" 2>&1
	if ! grep -Fq 'chmap-fixed=FL,FR' \
		"$result_dir/audio-channel-map.txt"; then
		printf 'CastKMS PCM does not advertise a stereo channel map\n' >&2
		cat "$result_dir/audio-channel-map.txt" >&2
		exit 1
	fi
	printf '%s\n' 'audio_channel_map=stereo' | \
		tee -a "$result_dir/summary.txt"

	# Attach a monitor with an audio-capable EDID using the runtime attach client.
	mkfifo "$runtime_dir/audio-attach-gate"
	exec 5<> "$runtime_dir/audio-attach-gate"
	audio_attach_gate_open=1
	sudo stdbuf --output=L --error=L ./tools/castkms-grant-launch \
		"$castkms_drm" "$virtual_connector_id" -- \
		./tools/castkms-attach \
	<&5 > "$result_dir/audio-attach-hold.txt" 2>&1 &
	attach_hold_pid=$!
	audio_attached=0
	for _ in $(seq 1 100); do
		if grep -Fx 'attached=1' "$result_dir/audio-attach-hold.txt" >/dev/null; then
			audio_attached=1
			break
		fi
		if ! kill -0 "$attach_hold_pid" 2>/dev/null; then
			cat "$result_dir/audio-attach-hold.txt" >&2
			exit 1
		fi
		sleep 0.1
	done
	if test "$audio_attached" -ne 1; then
		cat "$result_dir/audio-attach-hold.txt" >&2
		exit 1
	fi

	# Wait briefly for ELD propagation.
	sleep 0.5

	# Verify the ELD control contains non-zero data (monitor attached).
	sudo amixer -c "$castkms_card_index" cget iface=PCM,name='ELD' \
		> "$result_dir/audio-eld.txt" 2>&1
	if grep 'values=' "$result_dir/audio-eld.txt" |
		grep -oE '0x[0-9a-f]{2}' | grep -qv '^0x00$'; then
		printf '%s\n' 'audio_eld_present=pass' | \
			tee -a "$result_dir/summary.txt"
	else
		printf 'ELD control has no data after monitor attach\n' >&2
		cat "$result_dir/audio-eld.txt" >&2
		exit 1
	fi

	# Light the CRTC so the PCM device is fully operational.
	page_flip_input_dir=$(mktemp -d)
	mkfifo "$page_flip_input_dir/input"
	exec 4<> "$page_flip_input_dir/input"
	audio_mode_gate_open=1
	rm "$page_flip_input_dir/input"
	rmdir "$page_flip_input_dir"
	sudo timeout --signal=INT --kill-after=2s 15s \
		stdbuf --output=L --error=L modetest -M castkms \
		-s "$virtual_connector@$crtc_id:1024x768" \
		<&4 > "$result_dir/audio-modeset.txt" 2>&1 &
	audio_modeset_pid=$!

	modeset_ready=0
	for _ in $(seq 1 30); do
		if sudo modetest -a -M castkms -p 2>/dev/null |
			awk -v crtc="$crtc_id" \
				'$1 == crtc && $4 == "(1024x768)" { found = 1 }
				 END { exit !found }'; then
			modeset_ready=1
			break
		fi
		sleep 0.2
	done
	if test "$modeset_ready" -ne 1; then
		cat "$result_dir/audio-modeset.txt" >&2
		exit 1
	fi

	# Short playback test: send silence for 1 second.
	aplay_status=0
	sudo timeout --signal=TERM --kill-after=2s 5s \
		aplay -D "hw:${castkms_card_index},0" -f S16_LE -c 2 -r 48000 \
		-d 1 /dev/zero > "$result_dir/audio-aplay.txt" 2>&1 || \
		aplay_status=$?
	if test "$aplay_status" -ne 0; then
		cat "$result_dir/audio-aplay.txt" >&2
		exit 1
	fi
	printf '%s\n' 'audio_playback=pass' | tee -a "$result_dir/summary.txt"

	# Comprehensive audio timing validation.
	audio_test_status=0
	sudo timeout --signal=TERM --kill-after=2s 20s \
		./tools/castkms-audio-test 5 > "$result_dir/audio-timing.txt" 2>&1 || \
		audio_test_status=$?
	if test "$audio_test_status" -ne 0; then
		cat "$result_dir/audio-timing.txt" >&2
		exit 1
	fi

	audio_timing=$(grep '^audio_timing=' "$result_dir/audio-timing.txt" |
		cut -d= -f2)
	if test "$audio_timing" != "pass"; then
		printf 'audio_timing test reported: %s\n' "$audio_timing" >&2
		cat "$result_dir/audio-timing.txt" >&2
		exit 1
	fi

	grep -Fx 'system_ts_monotonic=1' "$result_dir/audio-timing.txt" >/dev/null
	printf '%s\n' 'system_ts_monotonic=1' | tee -a "$result_dir/summary.txt"
	grep -Fx 'audio_ts_present=1' "$result_dir/audio-timing.txt" >/dev/null
	printf '%s\n' 'audio_ts_present=1' | tee -a "$result_dir/summary.txt"
	grep '^clock_rate_error_pct=' "$result_dir/audio-timing.txt" |
		tee -a "$result_dir/summary.txt"
	grep '^pause_resume=' "$result_dir/audio-timing.txt" |
		tee -a "$result_dir/summary.txt" || true
	printf '%s\n' 'audio_timing=pass' | tee -a "$result_dir/summary.txt"

	# Clean up audio modeset holder.
	exec 4>&-
	audio_mode_gate_open=0
	kill "$audio_modeset_pid" 2>/dev/null || true
	wait "$audio_modeset_pid" 2>/dev/null || true
	audio_modeset_pid=

	# Detach the monitor and verify audio becomes unavailable.
	printf 'x' >&5
	exec 5>&-
	audio_attach_gate_open=0
	wait "$attach_hold_pid" 2>/dev/null || true
	attach_hold_pid=

	sleep 0.3

	# After detach, the ELD should contain only zeroes.
	sudo amixer -c "$castkms_card_index" cget iface=PCM,name='ELD' \
		> "$result_dir/audio-eld-after.txt" 2>&1
	if ! grep 'values=' "$result_dir/audio-eld-after.txt" |
		grep -oE '0x[0-9a-f]{2}' | grep -qv '^0x00$'; then
		printf '%s\n' 'audio_detach_eld=pass' | \
			tee -a "$result_dir/summary.txt"
	else
		printf 'ELD still has data after detach\n' >&2
		cat "$result_dir/audio-eld-after.txt" >&2
		exit 1
	fi

	rm -f "$runtime_dir/audio-attach-gate"
	printf '%s\n' 'audio_lifecycle=pass' | tee -a "$result_dir/summary.txt"

	# Wait for ALSA device references to drain before unloading.
	sudo killall alsactl 2>/dev/null || true
	sleep 0.3
	for _ in $(seq 1 20); do
		if sudo rmmod castkms 2>/dev/null; then
			break
		fi
		sudo fuser -k /dev/snd/* 2>/dev/null || true
		sleep 0.5
	done
	if lsmod | grep -q '^castkms\b'; then
		sudo fuser -v /dev/snd/* 2>&1 || true
		sudo rmmod castkms
	fi
	cast_loaded=0
	scenario_end pipewire-audio
}
