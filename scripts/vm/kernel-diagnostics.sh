#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only

# Shared kernel diagnostic gate for guest-side VM tests.

castkms_kernel_error_pattern='BUG:|kernel BUG at|WARNING:|Oops:|KASAN:|KCSAN:|'
castkms_kernel_error_pattern+='KMSAN:|UBSAN:|refcount_t:|use-after-free|'
castkms_kernel_error_pattern+='general protection fault|possible circular locking dependency|'
castkms_kernel_error_pattern+='\*ERROR\*'

castkms_capture_guest_provenance()
{
	local result_dir=$1
	local kernel_config

	kernel_config=/boot/config-$(uname -r)

	{
		printf 'kernel_release=%s\n' "$(uname -r)"
		printf 'machine=%s\n' "$(uname -m)"
		if test -r /etc/os-release; then
			grep -E '^(ID|VERSION_ID|BUILD_ID)=' /etc/os-release || true
		fi
	} > "$result_dir/guest-system.txt"

	LC_ALL=C rpm -qa \
		--qf '%{NAME}-%{EPOCHNUM}:%{VERSION}-%{RELEASE}.%{ARCH}\n' | \
		LC_ALL=C sort > "$result_dir/guest-packages.txt"

	if test -r "$kernel_config"; then
		grep -E \
			'^(CONFIG_(KASAN|KCSAN|KMSAN|UBSAN|PROVE_LOCKING|DEBUG_LOCK_ALLOC|DEBUG_SPINLOCK|DEBUG_MUTEXES|DEBUG_ATOMIC_SLEEP)=|# CONFIG_(KASAN|KCSAN|KMSAN|UBSAN|PROVE_LOCKING|DEBUG_LOCK_ALLOC|DEBUG_SPINLOCK|DEBUG_MUTEXES|DEBUG_ATOMIC_SLEEP) is not set)' \
			"$kernel_config" > "$result_dir/kernel-debug-features.txt"
	fi

	for manifest in /var/lib/castkms-vm/*-packages.txt; do
		if test -r "$manifest"; then
			cp "$manifest" "$result_dir/${manifest##*/}"
		fi
	done
}

castkms_capture_kernel_log()
{
	local log_file=$1

	sudo dmesg | tee "$log_file" >/dev/null
}

castkms_check_kernel_log()
{
	local log_file=$1
	local error_file=$2

	if grep -E "$castkms_kernel_error_pattern" "$log_file" > "$error_file"; then
		cat "$error_file" >&2
		return 1
	fi
	rm -f "$error_file"
}
