#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd)
drm_device=${1:?usage: $0 /dev/dri/cardN [crtc-id]}
crtc_id=${2:-}
result_dir=${3:-$script_dir/../../test-results/pw-castkms}
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

if ! pw-cli info 0 >/dev/null 2>&1; then
	XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
	export XDG_RUNTIME_DIR
	pipewire > "$result_dir/pipewire.log" 2>&1 &
	pipewire_pid=$!
	pipewire_started=1
	for attempt in $(seq 1 20); do
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

"$script_dir/pw-castkms" -d "$drm_device" ${crtc_id:+-c "$crtc_id"} \
	> "$result_dir/pw-castkms.log" 2>&1 &
bridge_pid=$!
for attempt in $(seq 1 20); do
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

if ! grep -Fx 'pw_castkms_test=pass' \
		"$result_dir/pw-castkms-test.txt" >/dev/null; then
	printf 'pw-castkms frame delivery test failed\n' >&2
	cat "$result_dir/pw-castkms.log" >&2
	exit 1
fi
printf '%s\n' 'pw_castkms_frame_delivery=pass' | tee -a "$result_dir/summary.txt"

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
