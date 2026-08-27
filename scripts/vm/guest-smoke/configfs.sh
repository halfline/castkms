# SPDX-License-Identifier: GPL-2.0-only
# shellcheck shell=bash

run_configfs_scenario()
{
	scenario_begin configfs
	sudo modprobe vkms create_default_dev=0
	stock_loaded=1
	sudo insmod ./castkms.ko \
		create_default_dev=0 enable_audio=0 enable_configfs=1
	cast_loaded=1

	test -d /sys/kernel/config/vkms
	test -d /sys/kernel/config/castkms
	lsmod | grep -E '^(vkms|castkms)\b' | \
		tee "$result_dir/coexistence-modules.txt"
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
	for _ in $(seq 1 50); do
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
	configfs_disabled=0
	for _ in $(seq 1 50); do
		if test "$(sudo cat "$configfs_dev/enabled")" = 0; then
			configfs_disabled=1
			break
		fi
		sleep 0.1
	done
	if test "$configfs_disabled" -ne 1; then
		printf '%s\n' 'configfs device did not disable after topology removal' >&2
		exit 1
	fi
	printf '%s\n' 'configfs_topology_lifetime=pass' | \
		tee -a "$result_dir/summary.txt"

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
	printf '%s\n' 'configfs_open_fd_lifetime=pass' | \
		tee -a "$result_dir/summary.txt"

	sudo rmmod castkms
	cast_loaded=0
	sudo rmmod vkms
	stock_loaded=0
	scenario_end configfs
}
