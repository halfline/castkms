#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

repo_dir=${1:-$HOME/castkms}
expected_release=${2:?missing expected kernel release}
result_dir=$repo_dir/test-results/vm-smoke
stock_loaded=0
cast_loaded=0
configfs_dev=/sys/kernel/config/castkms/lifetime-test
runtime_dir=
unplug_gate_open=0
unplug_helper_pid=
mode_gate_open=0
mode_holder_pid=
attach_gate_open=0
attach_hold_pid=
connect_modeset_pid=
crc_fd=
crc_pid=
writeback_pid=
pw_daemon_pid=
pw_wireplumber_pid=
pw_source_pid=
pw_modeset_pid=
pw_mode_gate_open=0
pw_runtime=
sink_capture_pid=

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

cleanup()
{
	if test -d "$configfs_dev"; then
		sudo rm -f \
			"$configfs_dev/planes/primary-0/possible_crtcs/crtc-0" \
			"$configfs_dev/encoders/encoder-0/possible_crtcs/crtc-0" \
			"$configfs_dev/connectors/connector-0/possible_encoders/encoder-0"
		sudo rmdir "$configfs_dev/connectors/connector-0" 2>/dev/null || true
		sudo rmdir "$configfs_dev/encoders/encoder-0" 2>/dev/null || true
		sudo rmdir "$configfs_dev/planes/primary-0" 2>/dev/null || true
		sudo rmdir "$configfs_dev/crtcs/crtc-0" 2>/dev/null || true
		sudo rmdir "$configfs_dev" 2>/dev/null || true
	fi
	if test -n "$crc_fd"; then
		exec {crc_fd}<&-
		crc_fd=
	fi
	if test -n "$crc_pid"; then
		kill "$crc_pid" 2>/dev/null || true
		wait "$crc_pid" 2>/dev/null || true
	fi
	if test -n "$writeback_pid"; then
		kill "$writeback_pid" 2>/dev/null || true
		wait "$writeback_pid" 2>/dev/null || true
	fi
	if test "$mode_gate_open" -eq 1; then
		printf '\n' >&8 2>/dev/null || true
		exec 8>&-
	fi
	if test -n "$mode_holder_pid"; then
		kill "$mode_holder_pid" 2>/dev/null || true
		wait "$mode_holder_pid" 2>/dev/null || true
	fi
	if test "$attach_gate_open" -eq 1; then
		printf 'x' >&9 2>/dev/null || true
		exec 9>&-
	fi
	if test -n "$attach_hold_pid"; then
		kill "$attach_hold_pid" 2>/dev/null || true
		wait "$attach_hold_pid" 2>/dev/null || true
	fi
	if test -n "$connect_modeset_pid"; then
		kill "$connect_modeset_pid" 2>/dev/null || true
		wait "$connect_modeset_pid" 2>/dev/null || true
	fi
	if test "$unplug_gate_open" -eq 1; then
		printf 'x' >&7 2>/dev/null || true
		exec 7>&-
	fi
	if test -n "$unplug_helper_pid"; then
		kill "$unplug_helper_pid" 2>/dev/null || true
		wait "$unplug_helper_pid" 2>/dev/null || true
	fi
	if test "$pw_mode_gate_open" -eq 1; then
		exec 6>&-
	fi
	if test -n "$sink_capture_pid"; then
		kill "$sink_capture_pid" 2>/dev/null || true
		wait "$sink_capture_pid" 2>/dev/null || true
	fi
	if test -n "$pw_source_pid"; then
		kill "$pw_source_pid" 2>/dev/null || true
		wait "$pw_source_pid" 2>/dev/null || true
	fi
	if test -n "$pw_modeset_pid"; then
		kill "$pw_modeset_pid" 2>/dev/null || true
		wait "$pw_modeset_pid" 2>/dev/null || true
	fi
	if test -n "$pw_wireplumber_pid"; then
		kill "$pw_wireplumber_pid" 2>/dev/null || true
		wait "$pw_wireplumber_pid" 2>/dev/null || true
	fi
	if test -n "$pw_daemon_pid"; then
		kill "$pw_daemon_pid" 2>/dev/null || true
		wait "$pw_daemon_pid" 2>/dev/null || true
	fi
	if test -n "$pw_runtime"; then
		sudo rm -rf "$pw_runtime"
	fi
	if test -n "$runtime_dir"; then
		rm -f "$runtime_dir/drm-unplug-check" \
			"$runtime_dir/unplug-gate" "$runtime_dir/mode-gate" \
			"$runtime_dir/attach-gate" \
			"$runtime_dir/pw-mode-gate"
		rmdir "$runtime_dir" 2>/dev/null || true
	fi
	if test "$cast_loaded" -eq 1; then
		sudo rmmod castkms || true
	fi
	if test "$stock_loaded" -eq 1; then
		sudo rmmod vkms || true
	fi
}

trap cleanup EXIT

mkdir -p "$result_dir"
cd "$repo_dir"
runtime_dir=$(mktemp -d)

running_release=$(uname -r)
test "$running_release" = "$expected_release"
printf 'kernel=%s\n' "$running_release" | tee "$result_dir/summary.txt"

make clean
test ! -e ./castkms.ko
test ! -e ./src/tests/castkms-kunit-tests.ko
test ! -e ./tools/castkms-capture-test
test ! -e ./tools/castkms-audio-test
test ! -e ./tools/pw-castkms/pw-castkms
test ! -e ./tools/pw-castkms/pw-castkms-test
make kunit W=1 2>&1 | tee "$result_dir/build.log"

test "$(modinfo -F name ./castkms.ko)" = castkms
case "$(modinfo -F vermagic ./castkms.ko)" in
	"$expected_release "*) ;;
	*) printf '%s\n' 'module vermagic does not match the guest kernel' >&2; exit 1 ;;
esac

test "$(modinfo -F name ./src/tests/castkms-kunit-tests.ko)" = \
	castkms_kunit_tests
case "$(modinfo -F vermagic ./src/tests/castkms-kunit-tests.ko)" in
	"$expected_release "*) ;;
	*) printf '%s\n' 'KUnit module vermagic does not match the guest kernel' >&2; exit 1 ;;
esac
case ",$(modinfo -F depends ./src/tests/castkms-kunit-tests.ko)," in
	*,castkms,*kunit,*|*,kunit,*castkms,*) ;;
	*) printf '%s\n' 'KUnit module dependencies are incomplete' >&2; exit 1 ;;
esac
printf '%s\n' 'kunit_build=pass' | tee -a "$result_dir/summary.txt"
"$repo_dir/scripts/vm/guest-kunit-test.sh" "$repo_dir" "$expected_release"
grep -Fx 'result=pass' "$repo_dir/test-results/vm-kunit/summary.txt" >/dev/null
cp "$repo_dir/test-results/vm-kunit/summary.txt" \
	"$result_dir/kunit-summary.txt"
printf '%s\n' 'kunit_run=pass' | tee -a "$result_dir/summary.txt"

make tools 2>&1 | tee "$result_dir/tools-build.log"
test -x ./tools/castkms-capture-test
test -x ./tools/castkms-audio-test
test -x ./tools/pw-castkms/pw-castkms
test -x ./tools/pw-castkms/pw-castkms-test
printf '%s\n' 'capture_probe_build=pass' | tee -a "$result_dir/summary.txt"

if ! strings ./castkms.ko > "$result_dir/module-strings.txt"; then
	printf '%s\n' 'could not inspect the module string table' >&2
	exit 1
fi
if grep -i vkms "$result_dir/module-strings.txt" \
		> "$result_dir/legacy-strings.txt"; then
	printf '%s\n' 'legacy VKMS identity remains in castkms.ko' >&2
	exit 1
fi

if awk '{ print $2 }' Module.symvers | grep -Ev '^castkms_' > "$result_dir/non-castkms-exports.txt"; then
	printf '%s\n' 'module contains non-namespaced exports' >&2
	exit 1
fi

if lsmod | grep -Eq '^(vkms|castkms)\b'; then
	printf '%s\n' 'refusing to disturb a pre-existing vkms or castkms module' >&2
	exit 1
fi

if ! mountpoint -q /sys/kernel/config; then
	sudo mount -t configfs none /sys/kernel/config
fi
if ! mountpoint -q /sys/kernel/debug; then
	sudo mount -t debugfs none /sys/kernel/debug
fi

sudo modprobe snd-pcm
sudo modprobe vkms create_default_dev=0
stock_loaded=1
sudo insmod ./castkms.ko create_default_dev=0 enable_audio=0
cast_loaded=1

test -d /sys/kernel/config/vkms
test -d /sys/kernel/config/castkms
lsmod | grep -E '^(vkms|castkms)\b' | tee "$result_dir/coexistence-modules.txt"
ls -ld /sys/kernel/config/vkms /sys/kernel/config/castkms | \
	tee "$result_dir/coexistence-configfs.txt"

sudo mkdir "$configfs_dev"
sudo mkdir "$configfs_dev/planes/primary-0"
sudo mkdir "$configfs_dev/crtcs/crtc-0"
sudo mkdir "$configfs_dev/encoders/encoder-0"
sudo mkdir "$configfs_dev/connectors/connector-0"
printf '1\n' | sudo tee "$configfs_dev/planes/primary-0/type" >/dev/null
sudo ln -s "$configfs_dev/crtcs/crtc-0" \
	"$configfs_dev/planes/primary-0/possible_crtcs/crtc-0"
sudo ln -s "$configfs_dev/crtcs/crtc-0" \
	"$configfs_dev/encoders/encoder-0/possible_crtcs/crtc-0"
sudo ln -s "$configfs_dev/encoders/encoder-0" \
	"$configfs_dev/connectors/connector-0/possible_encoders/encoder-0"
printf '1\n' | sudo tee "$configfs_dev/enabled" >/dev/null
test "$(sudo cat "$configfs_dev/enabled")" = 1
sudo udevadm settle

lifetime_debugfs=/sys/kernel/debug/dri/lifetime-test
sudo test -r "$lifetime_debugfs/castkms_config"
lifetime_minor=$(sudo find /sys/kernel/debug/dri -maxdepth 1 -type l \
	-lname lifetime-test -printf '%f\n')
test -n "$lifetime_minor"
lifetime_drm=/dev/dri/card$lifetime_minor
test -c "$lifetime_drm"

cc -std=gnu11 -O2 -Wall -Wextra -Werror \
	-o "$runtime_dir/drm-unplug-check" \
	"$repo_dir/scripts/vm/drm-unplug-check.c"
mkfifo "$runtime_dir/unplug-gate"
exec 7<> "$runtime_dir/unplug-gate"
unplug_gate_open=1

# Keep both userspace-facing descriptors open until the complete configfs
# object has been removed, then require both interfaces to report unplugging.
sudo timeout --signal=TERM --kill-after=2s 15s \
	"$runtime_dir/drm-unplug-check" \
	"$lifetime_drm" "$lifetime_debugfs/castkms_config" \
	<&7 > "$result_dir/configfs-open-fd.txt" 2>&1 &
unplug_helper_pid=$!
for attempt in $(seq 1 50); do
	if grep -Fx 'ready=castkms' "$result_dir/configfs-open-fd.txt" \
			>/dev/null; then
		break
	fi
	if ! kill -0 "$unplug_helper_pid" 2>/dev/null; then
		cat "$result_dir/configfs-open-fd.txt" >&2
		exit 1
	fi
	sleep 0.1
done
if ! grep -Fx 'ready=castkms' "$result_dir/configfs-open-fd.txt" \
		>/dev/null; then
	cat "$result_dir/configfs-open-fd.txt" >&2
	exit 1
fi

# Removing topology cannot be rejected by configfs. It must first disable the
# live DRM device so no runtime object retains the detached configuration.
sudo rm "$configfs_dev/planes/primary-0/possible_crtcs/crtc-0"
test "$(sudo cat "$configfs_dev/enabled")" = 0
printf '%s\n' 'configfs_topology_lifetime=pass' | tee -a "$result_dir/summary.txt"

sudo rm "$configfs_dev/encoders/encoder-0/possible_crtcs/crtc-0"
sudo rm "$configfs_dev/connectors/connector-0/possible_encoders/encoder-0"
sudo rmdir "$configfs_dev/connectors/connector-0"
sudo rmdir "$configfs_dev/encoders/encoder-0"
sudo rmdir "$configfs_dev/planes/primary-0"
sudo rmdir "$configfs_dev/crtcs/crtc-0"
sudo rmdir "$configfs_dev"
printf 'x' >&7
exec 7>&-
unplug_gate_open=0
if ! wait "$unplug_helper_pid"; then
	cat "$result_dir/configfs-open-fd.txt" >&2
	exit 1
fi
unplug_helper_pid=
grep -Fx 'drm_ioctl_after_unplug=ENODEV' \
	"$result_dir/configfs-open-fd.txt" >/dev/null
grep -Fx 'debugfs_read_after_unplug=EIO' \
	"$result_dir/configfs-open-fd.txt" >/dev/null
printf '%s\n' 'configfs_open_fd_lifetime=pass' | tee -a "$result_dir/summary.txt"

sudo rmmod castkms
cast_loaded=0
sudo rmmod vkms
stock_loaded=0

sudo insmod ./castkms.ko \
	create_default_dev=1 \
	enable_cursor=0 \
	enable_overlay=0 \
	enable_writeback=1 \
	enable_plane_pipeline=1 \
	enable_audio=0
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
sudo stdbuf --output=L --error=L bash -c '
	virtual_connector=$1
	crtc_id=$2
	log=$3
	for attempt in $(seq 1 80); do
		if sudo modetest -M castkms -c 2>/dev/null |
			awk -v name="$virtual_connector" \
				"\$4 == name && \$3 == \"connected\" { found = 1 }
				 END { exit !found }"; then
			exec sudo timeout --signal=INT --kill-after=2s 60s \
				stdbuf --output=L --error=L \
				modetest -M castkms \
				-s "$virtual_connector@$crtc_id:1024x768"
		fi
		sleep 0.1
	done
	printf "timed out waiting for attached connector\\n" >"$log"
	exit 1
' _ "$virtual_connector" "$crtc_id" "$result_dir/connect-modeset.txt" \
	> "$result_dir/connect-modeset.txt" 2>&1 &
connect_modeset_pid=$!

sudo ./tools/castkms-capture-test "$castkms_drm" "$crtc_id" | \
	tee "$result_dir/capture-test.txt"
grep -Fx 'drm_cap_syncobj=1' "$result_dir/capture-test.txt" >/dev/null
grep -Fx 'drm_cap_syncobj_timeline=1' \
	"$result_dir/capture-test.txt" >/dev/null
grep -Fx 'capture_non_master=1' "$result_dir/capture-test.txt" >/dev/null
grep -Fx 'capture_uapi=0.8' "$result_dir/capture-test.txt" >/dev/null
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
grep -Fx 'capture_buffer_postclose=pass' \
	"$result_dir/capture-test.txt" >/dev/null
grep -Fx 'capture_buffer_registration=pass' \
	"$result_dir/capture-test.txt" >/dev/null
grep -Fx 'capture_stream_exclusive=pass' \
	"$result_dir/capture-test.txt" >/dev/null
grep -Fx 'capture_attach_monitor=pass' \
	"$result_dir/capture-test.txt" >/dev/null
grep -Fx 'capture_output_edid=pass' \
	"$result_dir/capture-test.txt" >/dev/null
grep -Fx 'capture_stream_stop=pass' \
	"$result_dir/capture-test.txt" >/dev/null
grep -Fx 'capture_stream_postclose=pass' \
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

if test -n "$connect_modeset_pid"; then
	kill "$connect_modeset_pid" 2>/dev/null || true
	wait "$connect_modeset_pid" 2>/dev/null || true
	connect_modeset_pid=
fi

# Hold a connected monitor so later modeset/writeback jobs see a sink.
mkfifo "$runtime_dir/attach-gate"
exec 9<> "$runtime_dir/attach-gate"
attach_gate_open=1
sudo stdbuf --output=L --error=L \
	./tools/castkms-capture-test --attach "$castkms_drm" "$crtc_id" \
	<&9 > "$result_dir/attach-hold.txt" 2>&1 &
attach_hold_pid=$!
attach_ready=0
for attempt in $(seq 1 50); do
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
rm "$page_flip_input_dir/input"
rmdir "$page_flip_input_dir"

page_flip_status=0
sudo timeout --signal=INT --kill-after=2s 5s \
	stdbuf --output=L --error=L modetest -M castkms \
	-s "$virtual_connector@$crtc_id:800x600" -v \
	<&3 > "$result_dir/page-flip.txt" 2>&1 || \
	page_flip_status=$?
exec 3>&-
if test "$page_flip_status" -ne 124 ||
	! grep -q '^freq:' "$result_dir/page-flip.txt"; then
	cat "$result_dir/page-flip.txt" >&2
	exit 1
fi
sudo ./tools/castkms-capture-test --mode-generation "$castkms_drm" \
	"$crtc_id" > "$result_dir/capture-test-after-modeset.txt"
updated_capture_mode_generation=$(sed -n \
	's/^capture_mode_generation=//p' \
	"$result_dir/capture-test-after-modeset.txt")
[[ $updated_capture_mode_generation =~ ^[0-9]+$ ]]
test "$updated_capture_mode_generation" -gt \
	"$initial_capture_mode_generation"
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
for attempt in $(seq 1 50); do
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
for sample in 1 2 3; do
	append_crc_record "$result_dir/crc.txt" baseline
done
printf '%s\n' 'composer_crc=pass' | tee -a "$result_dir/summary.txt"

capture_overlap_status=0
sudo stdbuf --output=L --error=L \
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
for attempt in $(seq 1 50); do
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
for sample in 1 2 3; do
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

# Reload with cursor support for cursor metadata tests
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
test -n "$crtc_id"

sudo ./tools/castkms-capture-test --cursor "$castkms_drm" "$crtc_id" | \
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

# PipeWire bridge test
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
for attempt in $(seq 1 20); do
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
	./tools/pw-castkms/pw-castkms -d "$castkms_drm" -c "$crtc_id" \
	> "$result_dir/pw-castkms.txt" 2>&1 &
pw_source_pid=$!

pw_attached=0
for attempt in $(seq 1 50); do
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
for attempt in $(seq 1 60); do
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

# PipeWire audio sink discovery — WirePlumber should expose the CastKMS ALSA
# card as an Audio/Sink while the audio-capable monitor is attached.
pw_audio_sink=0
for attempt in $(seq 1 20); do
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

# Sink monitor audio capture — verify that audio data played to the CastKMS
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
sudo rm -rf "$pw_runtime"
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
crtc_id=$(awk '
	$0 == "CRTCs:" { in_crtcs = 1; next }
	in_crtcs && $1 ~ /^[0-9]+$/ { print $1; exit }
' "$result_dir/audio-modetest.txt")
test -n "$virtual_connector"
test -n "$crtc_id"

# Before attach, playback should fail (no audio-capable monitor).
castkms_card_index=$(awk '/CastKMS/ { print $1; exit }' /proc/asound/cards)
if test -z "$castkms_card_index"; then
	printf 'could not determine CastKMS card index\n' >&2
	exit 1
fi

# Attach a monitor with audio-capable EDID using castkms-capture-test --attach.
mkfifo "$runtime_dir/audio-attach-gate"
exec 5<> "$runtime_dir/audio-attach-gate"
sudo stdbuf --output=L --error=L \
	./tools/castkms-capture-test --attach "$castkms_drm" "$crtc_id" \
	<&5 > "$result_dir/audio-attach-hold.txt" 2>&1 &
attach_hold_pid=$!
audio_attached=0
for attempt in $(seq 1 50); do
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
rm "$page_flip_input_dir/input"
rmdir "$page_flip_input_dir"
sudo timeout --signal=INT --kill-after=2s 15s \
	stdbuf --output=L --error=L modetest -M castkms \
	-s "$virtual_connector@$crtc_id:1024x768" \
	<&4 > "$result_dir/audio-modeset.txt" 2>&1 &
audio_modeset_pid=$!

modeset_ready=0
for attempt in $(seq 1 30); do
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
kill "$audio_modeset_pid" 2>/dev/null || true
wait "$audio_modeset_pid" 2>/dev/null || true
audio_modeset_pid=

# Detach the monitor and verify audio becomes unavailable.
printf 'x' >&5
exec 5>&-
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
for attempt in $(seq 1 20); do
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

printf '%s\n' 'result=pass' | tee -a "$result_dir/summary.txt"
