#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

repo_dir=${1:-$HOME/castkms}
expected_release=${2:?missing expected kernel release}
result_dir=$repo_dir/test-results/vm-mutter
cast_loaded=0
keep_session=0
uid=$(id -u)

cleanup()
{
	if test "$keep_session" -eq 1; then
		return
	fi
	if test "$cast_loaded" -eq 1; then
		sudo systemctl stop gdm.service 2>/dev/null || true
		sleep 1
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
make W=1 2>&1 | tee "$result_dir/build.log"
test -f ./castkms.ko

if lsmod | grep -Eq '^(vkms|castkms)\b'; then
	printf '%s\n' 'refusing to disturb a pre-existing vkms or castkms module' >&2
	exit 1
fi

if ! mountpoint -q /sys/kernel/debug; then
	sudo mount -t debugfs none /sys/kernel/debug
fi

sudo systemctl stop gdm.service 2>/dev/null || true

sudo insmod ./castkms.ko \
	create_default_dev=1 \
	enable_cursor=0 \
	enable_overlay=0 \
	enable_writeback=0 \
	enable_plane_pipeline=0
cast_loaded=1
sudo udevadm settle

castkms_minor=$(sudo find /sys/kernel/debug/dri -maxdepth 1 -type l \
	-lname castkms -printf '%f\n')
test -n "$castkms_minor"
castkms_drm=/dev/dri/card$castkms_minor
test -c "$castkms_drm"

sudo udevadm info --query=property --name="$castkms_drm" \
	> "$result_dir/udev-castkms.txt"
if grep -E '^TAGS=.*mutter-device-ignore' \
		"$result_dir/udev-castkms.txt" >/dev/null ||
	grep -Fx 'ID_PATH=platform-vkms' \
		"$result_dir/udev-castkms.txt" >/dev/null; then
	printf '%s\n' 'castkms card is tagged mutter-device-ignore' >&2
	cat "$result_dir/udev-castkms.txt" >&2
	exit 1
fi
printf '%s\n' 'mutter_udev_ignore=absent' | tee -a "$result_dir/summary.txt"

sudo drm_info > "$result_dir/drm-info.txt" 2>&1 || true
if ! sudo modetest -a -M castkms -c \
		> "$result_dir/modetest.txt" 2>&1; then
	cat "$result_dir/modetest.txt" >&2
	exit 1
fi
virtual_connector=$(awk '
	$1 ~ /^[0-9]+$/ && $4 ~ /^Virtual-/ { print $4; exit }
' "$result_dir/modetest.txt")
test -n "$virtual_connector"
printf 'virtual_connector=%s\n' "$virtual_connector" | \
	tee -a "$result_dir/summary.txt"

sudo systemctl start gdm.service

export XDG_RUNTIME_DIR=/run/user/$uid
export DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus

session_ready=0
for attempt in $(seq 1 90); do
	if test -S "$XDG_RUNTIME_DIR/bus" &&
		busctl --user status org.gnome.Shell >/dev/null 2>&1 &&
		busctl --user status org.gnome.Mutter.DisplayConfig \
			>/dev/null 2>&1; then
		session_ready=1
		break
	fi
	sleep 2
done
if test "$session_ready" -ne 1; then
	printf '%s\n' 'GNOME session did not become ready' >&2
	sudo journalctl -b -u gdm --no-pager -n 80 >&2 || true
	exit 1
fi
printf '%s\n' 'gnome_session=ready' | tee -a "$result_dir/summary.txt"

gdbus call --session \
	--dest org.gnome.Mutter.DisplayConfig \
	--object-path /org/gnome/Mutter/DisplayConfig \
	--method org.gnome.Mutter.DisplayConfig.GetCurrentState \
	> "$result_dir/mutter-display-config.txt"

if ! grep -F "$virtual_connector" \
		"$result_dir/mutter-display-config.txt" >/dev/null; then
	printf '%s\n' 'Mutter DisplayConfig does not list the castkms connector' >&2
	cat "$result_dir/mutter-display-config.txt" >&2
	exit 1
fi
printf '%s\n' 'mutter_display_config=pass' | tee -a "$result_dir/summary.txt"
printf '%s\n' 'result=pass' | tee -a "$result_dir/summary.txt"
keep_session=1
