#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

repo_dir=${1:-$HOME/castkms}
expected_release=${2:?missing expected kernel release}
result_dir=$repo_dir/test-results/vm-kunit
cast_loaded=0
kunit_tests_loaded=0

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
cd "$repo_dir"

running_release=$(uname -r)
test "$running_release" = "$expected_release"
printf 'kernel=%s\n' "$running_release" | tee "$result_dir/summary.txt"

make clean
make kunit W=1 2>&1 | tee "$result_dir/build.log"
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
sudo insmod ./castkms.ko create_default_dev=0
cast_loaded=1
sudo insmod ./src/tests/castkms-kunit-tests.ko
kunit_tests_loaded=1

sudo dmesg > "$result_dir/dmesg.txt"

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

if test "$suites" -eq 0; then
	printf '%s\n' 'no KUnit test suites found in dmesg' >&2
	exit 1
fi

if test "$suite_failures" -ne 0; then
	printf '%s\n' "kunit=$suite_failures suite(s) had failures" | \
		tee -a "$result_dir/summary.txt"
	grep -E 'not ok [0-9]+ castkms' "$result_dir/dmesg.txt" | \
		tee "$result_dir/failures.txt" >&2
	exit 1
fi

printf '%s\n' 'result=pass' | tee -a "$result_dir/summary.txt"
