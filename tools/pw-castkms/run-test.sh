#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd)
drm_device=${1:?usage: $0 /dev/dri/cardN [crtc-id]}
crtc_id=${2:-}
result_dir=${3:-$script_dir/../../test-results/pw-castkms}
grant_launcher=$script_dir/../castkms-grant-launch
bridge_pid=
pipewire_started=0
pipewire_pid=

cleanup()
{
	if test -n "$bridge_pid"; then
		kill "$bridge_pid" 2>/dev/null || true
		wait "$bridge_pid" 2>/dev/null || true
	fi
	if test "$pipewire_started" -eq 1 && test -n "$pipewire_pid"; then
		kill "$pipewire_pid" 2>/dev/null || true
		wait "$pipewire_pid" 2>/dev/null || true
	fi
}
trap cleanup EXIT

mkdir -p "$result_dir"

if ! test -x "$script_dir/pw-castkms" ||
	! test -x "$script_dir/pw-castkms-test"; then
	printf 'build pw-castkms and pw-castkms-test first\n' >&2
	exit 1
fi

if test -z "${CASTKMS_GRANT_FD:-}"; then
	if test "$EUID" -ne 0; then
		printf '%s\n' \
			'CASTKMS_GRANT_FD is required (or run this isolated lab test as root)' >&2
		exit 1
	fi
	if ! test -x "$grant_launcher"; then
		printf 'build castkms-grant-launch first\n' >&2
		exit 1
	fi
	connector_id=$(modetest -a -M castkms -c 2>/dev/null | awk '
		$1 ~ /^[0-9]+$/ && $4 ~ /^Virtual-/ { print $1; exit }
	')
	if test -z "$connector_id"; then
		printf 'could not find a CastKMS virtual connector\n' >&2
		exit 1
	fi
fi

if ! pw-cli info 0 >/dev/null 2>&1; then
	XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
	export XDG_RUNTIME_DIR
	pipewire > "$result_dir/pipewire.log" 2>&1 &
	pipewire_pid=$!
	pipewire_started=1
	for _ in $(seq 1 20); do
		if pw-cli info 0 >/dev/null 2>&1; then
			break
		fi
		if ! kill -0 "$pipewire_pid" 2>/dev/null; then
			printf 'PipeWire exited prematurely\n' >&2
			cat "$result_dir/pipewire.log" >&2
			exit 1
		fi
		sleep 0.25
	done
	if ! pw-cli info 0 >/dev/null 2>&1; then
		printf 'PipeWire did not start within 5 seconds\n' >&2
		exit 1
	fi
fi
printf '%s\n' 'pipewire=running' | tee "$result_dir/summary.txt"

if test -n "${CASTKMS_GRANT_FD:-}"; then
	"$script_dir/pw-castkms" -g "$CASTKMS_GRANT_FD" -U -d "$drm_device" \
		${crtc_id:+-c "$crtc_id"} \
		> "$result_dir/pw-castkms.log" 2>&1 &
else
	"$grant_launcher" "$drm_device" "$connector_id" -- \
		"$script_dir/pw-castkms" -U -d "$drm_device" \
		${crtc_id:+-c "$crtc_id"} \
		> "$result_dir/pw-castkms.log" 2>&1 &
fi
bridge_pid=$!
for _ in $(seq 1 20); do
	if ! kill -0 "$bridge_pid" 2>/dev/null; then
		printf 'pw-castkms exited prematurely\n' >&2
		cat "$result_dir/pw-castkms.log" >&2
		exit 1
	fi
	if grep -q '^stream: streaming' "$result_dir/pw-castkms.log" 2>/dev/null; then
		break
	fi
	sleep 0.25
done
if ! grep -q '^stream: streaming' "$result_dir/pw-castkms.log" 2>/dev/null; then
	printf 'pw-castkms did not reach streaming state\n' >&2
	cat "$result_dir/pw-castkms.log" >&2
	exit 1
fi
printf '%s\n' 'pw_castkms_bridge=running' | tee -a "$result_dir/summary.txt"

test_status=0
"$script_dir/pw-castkms-test" -f 30 -t 10 \
	| tee "$result_dir/pw-castkms-test.txt" || test_status=$?

if test "$test_status" -ne 0; then
	printf 'pw-castkms frame delivery exited with status %d\n' \
		"$test_status" >&2
	cat "$result_dir/pw-castkms.log" >&2
	exit 1
fi
if ! grep -Fx 'pw_castkms_test=pass' \
		"$result_dir/pw-castkms-test.txt" >/dev/null; then
	printf 'pw-castkms frame delivery test failed\n' >&2
	cat "$result_dir/pw-castkms.log" >&2
	exit 1
fi
printf '%s\n' 'pw_castkms_frame_delivery=pass' | tee -a "$result_dir/summary.txt"

# Disconnecting the first consumer removes PipeWire's destination pool.  The
# source should remain published and rebuild its CastKMS stream for a second
# consumer.
sleep 0.25
if ! kill -0 "$bridge_pid" 2>/dev/null; then
	printf 'pw-castkms exited after its first consumer disconnected\n' >&2
	cat "$result_dir/pw-castkms.log" >&2
	exit 1
fi
"$script_dir/pw-castkms-test" -f 10 -t 10 \
	| tee "$result_dir/pw-castkms-reconnect-test.txt"
sleep 0.25
if ! grep -Fx 'pw_castkms_test=pass' \
		"$result_dir/pw-castkms-reconnect-test.txt" >/dev/null ||
	! kill -0 "$bridge_pid" 2>/dev/null ||
	grep -Eq 'ownership mismatch|could not restart capture' \
		"$result_dir/pw-castkms.log"; then
	printf 'pw-castkms buffer-pool reconnect test failed\n' >&2
	cat "$result_dir/pw-castkms.log" >&2
	exit 1
fi
printf '%s\n' 'pw_castkms_pool_reconnect=pass' | \
	tee -a "$result_dir/summary.txt"

kill "$bridge_pid"
bridge_status=0
wait "$bridge_pid" || bridge_status=$?
bridge_pid=
# 143 = SIGTERM (128 + 15)
if test "$bridge_status" -ne 0 && test "$bridge_status" -ne 143; then
	printf 'pw-castkms exited with status %d\n' "$bridge_status" >&2
	cat "$result_dir/pw-castkms.log" >&2
	exit 1
fi
printf '%s\n' 'pw_castkms_shutdown=pass' | tee -a "$result_dir/summary.txt"

printf '%s\n' 'result=pass' | tee -a "$result_dir/summary.txt"
