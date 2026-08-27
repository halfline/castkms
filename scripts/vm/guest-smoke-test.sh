#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=kernel-diagnostics.sh
. "$script_dir/kernel-diagnostics.sh"
# shellcheck source=module-dependencies.sh
. "$script_dir/module-dependencies.sh"

repo_dir=${1:-$HOME/castkms}
expected_release=${2:?missing expected kernel release}
scenario=${3:-all}
result_dir=$repo_dir/test-results/vm-smoke
stock_loaded=0
cast_loaded=0
configfs_dev=/sys/kernel/config/castkms/lifetime-test
runtime_dir=
unplug_gate_open=0
unplug_helper_pid=
mode_gate_open=0
mode_holder_pid=
page_flip_gate_open=0
page_flip_pid=
attach_gate_open=0
attach_hold_pid=
connect_mode_gate_open=0
connect_modeset_pid=
crc_fd=
crc_pid=
# Scenario modules expose one run_*_scenario entrypoint each. The module graph
# has one authoritative loader; common.sh owns validation and ordered dispatch.
# shellcheck source=guest-smoke/modules.sh
. "$script_dir/guest-smoke/modules.sh"
smoke_validate_registry
smoke_validate_scenario "$scenario"

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
	if test "$mode_gate_open" -eq 1; then
		printf '\n' >&8 2>/dev/null || true
		exec 8>&-
	fi
	if test -n "$mode_holder_pid"; then
		kill "$mode_holder_pid" 2>/dev/null || true
		wait "$mode_holder_pid" 2>/dev/null || true
	fi
	if test "$page_flip_gate_open" -eq 1; then
		printf '\n' >&3 2>/dev/null || true
		exec 3>&-
	fi
	if test -n "$page_flip_pid"; then
		kill "$page_flip_pid" 2>/dev/null || true
		wait "$page_flip_pid" 2>/dev/null || true
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
	if test "$connect_mode_gate_open" -eq 1; then
		printf '\n' >&5 2>/dev/null || true
		exec 5>&-
	fi
	if test "$unplug_gate_open" -eq 1; then
		printf 'x' >&7 2>/dev/null || true
		exec 7>&-
	fi
	if test -n "$unplug_helper_pid"; then
		kill "$unplug_helper_pid" 2>/dev/null || true
		wait "$unplug_helper_pid" 2>/dev/null || true
	fi
	if test -n "$runtime_dir"; then
		rm -f "$runtime_dir/drm-unplug-check" \
			"$runtime_dir/unplug-gate" "$runtime_dir/mode-gate" \
			"$runtime_dir/attach-gate" "$runtime_dir/connect-mode-gate"
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
printf 'selected_scenario=%s\n' "$scenario" | tee -a "$result_dir/summary.txt"
castkms_capture_guest_provenance "$result_dir"

make clean
test ! -e ./castkms.ko
test ! -e ./src/tests/castkms-kunit-tests.ko
make kunit W=1 2>&1 | tee "$result_dir/build.log"
make -C tools castkms-attach castkms-capture-test \
	castkms-grant-launch castkms-cec-test 2>&1 | \
	tee "$result_dir/tools-build.log"
test -x ./tools/castkms-attach
test -x ./tools/castkms-capture-test
test -x ./tools/castkms-grant-launch
test -x ./tools/castkms-cec-test
printf '%s\n' 'capture_probe_build=pass' | tee -a "$result_dir/summary.txt"

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

load_module_dependencies ./castkms.ko

smoke_run_selected_scenarios "$scenario"
cleanup
trap - EXIT
castkms_capture_kernel_log "$result_dir/product-dmesg.txt"
castkms_check_kernel_log "$result_dir/product-dmesg.txt" \
	"$result_dir/product-kernel-errors.txt"
printf '%s\n' 'kernel_diagnostics=pass' | tee -a "$result_dir/summary.txt"
printf '%s\n' 'result=pass' | tee -a "$result_dir/summary.txt"
