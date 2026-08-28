#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=module-dependencies.sh
. "$script_dir/module-dependencies.sh"
# shellcheck source=kernel-diagnostics.sh
. "$script_dir/kernel-diagnostics.sh"

repo_dir=${1:-$HOME/castkms}
expected_release=${2:?missing expected kernel release}
result_dir=$repo_dir/test-results/vm-kunit
cast_loaded=0
kunit_tests_loaded=0
expected_suites=(
	castkms-capture-buffer
	castkms-cec-core
	castkms-core-client
	castkms-config
	castkms-format
	castkms-color
	castkms-frame-dispatch
	castkms-grant
	castkms-snapshot
)

cleanup()
{
	if test "$kunit_tests_loaded" -eq 1; then
		sudo rmmod castkms_kunit_tests || true
	fi
	if test "$cast_loaded" -eq 1; then
		sudo rmmod castkms || true
	fi
}

trap cleanup EXIT

mkdir -p "$result_dir"
find "$result_dir" -mindepth 1 -maxdepth 1 -type f -delete
cd "$repo_dir"

running_release=$(uname -r)
test "$running_release" = "$expected_release"
printf 'kernel=%s\n' "$running_release" | tee "$result_dir/summary.txt"
castkms_capture_guest_provenance "$result_dir"

make clean
make build-matrix W=1 2>&1 | tee "$result_dir/build.log"
make kunit W=1 2>&1 | tee -a "$result_dir/build.log"
test -f ./castkms.ko
test -f ./src/tests/castkms-kunit-tests.ko
printf '%s\n' 'build=pass' | tee -a "$result_dir/summary.txt"

if lsmod | grep -Eq '^castkms_kunit_tests\b'; then
	sudo rmmod castkms_kunit_tests
fi
if lsmod | grep -Eq '^castkms\b'; then
	sudo rmmod castkms
fi

sudo dmesg --clear
sudo modprobe kunit enable=1
load_module_dependencies ./castkms.ko
sudo insmod ./castkms.ko create_default_dev=0 enable_audio=0 enable_cec=0
cast_loaded=1
sudo insmod ./src/tests/castkms-kunit-tests.ko
kunit_tests_loaded=1

castkms_capture_kernel_log "$result_dir/dmesg.txt"

suites=0
suite_failures=0
while IFS= read -r line; do
	case "$line" in
		*'# Subtest: castkms-'*)
			suite=$(printf '%s' "$line" | sed 's/.*# Subtest: //')
			suites=$((suites + 1))
			;;
		*'# castkms-'*': pass:'*'fail:'*)
			pass=$(printf '%s' "$line" | sed 's/.*pass://;s/ .*//')
			fail=$(printf '%s' "$line" | sed 's/.*fail://;s/ .*//')
			total=$(printf '%s' "$line" | sed 's/.*total://;s/ .*//')
			printf 'suite=%s pass=%s fail=%s total=%s\n' \
				"$suite" "$pass" "$fail" "$total" | \
				tee -a "$result_dir/summary.txt"
			if test "$fail" -ne 0; then
				suite_failures=$((suite_failures + 1))
			fi
			;;
	esac
done < <(grep -E '# Subtest: castkms-|# castkms-.*: pass:' \
	"$result_dir/dmesg.txt")

for suite in "${expected_suites[@]}"; do
	if ! grep -Eq "\] ok [0-9]+ $suite$" \
		"$result_dir/dmesg.txt"; then
		printf 'missing or failed KUnit suite: %s\n' "$suite" | \
			tee -a "$result_dir/summary.txt" >&2
		suite_failures=$((suite_failures + 1))
	elif ! grep -Eq "# $suite: pass:" "$result_dir/dmesg.txt"; then
		printf 'suite=%s result=pass\n' "$suite" | \
			tee -a "$result_dir/summary.txt"
	fi
done

if test "$suites" -lt "${#expected_suites[@]}"; then
	printf 'kunit=found %s suites, expected at least %s\n' \
		"$suites" "${#expected_suites[@]}" | \
		tee -a "$result_dir/summary.txt" >&2
	suite_failures=$((suite_failures + 1))
fi

if grep -Eq 'not ok [0-9]+ castkms-' "$result_dir/dmesg.txt"; then
	suite_failures=$((suite_failures + 1))
fi

if test "$suite_failures" -ne 0; then
	printf '%s\n' "kunit=$suite_failures suite(s) had failures" | \
		tee -a "$result_dir/summary.txt"
	grep -E 'not ok [0-9]+ castkms' "$result_dir/dmesg.txt" | \
		tee "$result_dir/failures.txt" >&2
	exit 1
fi

castkms_check_kernel_log "$result_dir/dmesg.txt" \
	"$result_dir/kunit-kernel-errors.txt"

sudo rmmod castkms_kunit_tests
kunit_tests_loaded=0
sudo rmmod castkms
cast_loaded=0

make -C tools castkms-grant-test 2>&1 | tee "$result_dir/grant-build.log"

sudo dmesg --clear
load_module_dependencies ./castkms.ko
sudo insmod ./castkms.ko enable_audio=0 enable_cec=1 enable_writeback=1 \
	enable_crc=1 max_outputs=2
cast_loaded=1

card_path=
for candidate in /sys/class/drm/card[0-9]*; do
	device_path=$(readlink -f "$candidate/device" 2>/dev/null || true)
	if test -n "$device_path" && test "${device_path##*/}" = castkms; then
		card_path=/dev/dri/${candidate##*/}
		card_name=${candidate##*/}
		break
	fi
done
test -n "$card_path"

connector_ids=()
for candidate in "/sys/class/drm/$card_name"-Virtual-*; do
	if test -r "$candidate/connector_id"; then
		read -r connector_id < "$candidate/connector_id"
		connector_ids+=("$connector_id")
	fi
done
test "${#connector_ids[@]}" -eq 2

sudo ./tools/castkms-grant-test "$card_path" \
	"${connector_ids[0]}" "${connector_ids[1]}" 2>&1 | \
	tee "$result_dir/grant-live.log"
grep -Fxq 'grant_lifecycle=pass' "$result_dir/grant-live.log"
grep -Fxq 'grant_cross_connector_denied=pass' \
	"$result_dir/grant-live.log"
grep -Fxq 'grant_cross_connector_independent=pass' \
	"$result_dir/grant-live.log"
grep -Fxq 'grant_foreign_framebuffer_denied=pass' \
	"$result_dir/grant-live.log"
grep -Fxq 'grant_fdinfo=pass' "$result_dir/grant-live.log"
sudo rmmod castkms
cast_loaded=0
castkms_capture_kernel_log "$result_dir/grant-dmesg.txt"
castkms_check_kernel_log "$result_dir/grant-dmesg.txt" \
	"$result_dir/grant-kernel-errors.txt"

printf '%s\n' 'grant-live=pass' | tee -a "$result_dir/summary.txt"
printf '%s\n' 'result=pass' | tee -a "$result_dir/summary.txt"
