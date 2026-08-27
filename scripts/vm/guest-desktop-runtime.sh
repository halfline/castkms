#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Build the desktop guest runtime and (re)start the attach service.

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=module-dependencies.sh
. "$script_dir/module-dependencies.sh"

no_build=0
if test "${1:-}" = --no-build; then
	no_build=1
	shift
fi

repo_dir=${1:-$HOME/castkms}
unit_src=$repo_dir/scripts/vm/castkms-attach.service
unit_dst=/etc/systemd/system/castkms-attach.service
attach_src=$repo_dir/scripts/vm/guest-attach-monitor.sh
dependency_src=$repo_dir/scripts/vm/module-dependencies.sh

cd "$repo_dir"

if test "$no_build" -eq 0; then
	make -j"$(nproc)" W=1
	make -C tools -j"$(nproc)"
fi
test -f ./castkms.ko
test -x ./tools/castkms-attach
test -x ./tools/castkms-grant-launch
test -f "$unit_src"
test -x "$attach_src"
test -r "$dependency_src"

# systemd cannot load or exec user_home_t from a system unit.
sudo mkdir -p /usr/local/libexec /usr/local/lib/castkms
sudo install -m 0755 "$attach_src" /usr/local/libexec/castkms-attach-monitor
sudo install -m 0644 "$dependency_src" \
	/usr/local/libexec/castkms-module-dependencies
sudo install -m 0755 ./tools/castkms-attach \
	/usr/local/libexec/castkms-attach
sudo install -m 0755 ./tools/castkms-grant-launch \
	/usr/local/libexec/castkms-grant-launch
sudo install -m 0644 ./castkms.ko /usr/local/lib/castkms/castkms.ko
sudo install -m 0644 "$unit_src" "$unit_dst"
sudo restorecon -F /usr/local/libexec/castkms-attach-monitor \
	/usr/local/libexec/castkms-module-dependencies \
	/usr/local/libexec/castkms-attach \
	/usr/local/libexec/castkms-grant-launch \
	/usr/local/lib/castkms/castkms.ko \
	"$unit_dst" >/dev/null 2>&1 || true
sudo systemctl daemon-reload
sudo systemctl enable castkms-attach.service

# Serialize the boot-time unit with this runtime refresh before replacing or
# loading its module. Also take over a leftover interactive --attach hold.
sudo systemctl stop castkms-attach.service >/dev/null 2>&1 || true
sudo pkill -x castkms-attach >/dev/null 2>&1 || true
sleep 0.2

if ! lsmod | grep -Eq '^castkms\b'; then
	# insmod loads exactly one file and does not resolve the module's generated
	# dependency metadata. A clean desktop boot has neither dependency loaded.
	load_module_dependencies "$repo_dir/castkms.ko"
	sudo insmod /usr/local/lib/castkms/castkms.ko \
		create_default_dev=1 \
		enable_cursor=0 \
		enable_overlay=0 \
		enable_writeback=0 \
		enable_plane_pipeline=0
	sudo udevadm settle
fi

if systemctl is-enabled gdm.service >/dev/null 2>&1 &&
	! systemctl is-active --quiet gdm.service; then
	sudo systemctl start gdm.service
fi

sudo systemctl reset-failed castkms-attach.service
sudo systemctl restart castkms-attach.service

for attempt in $(seq 1 50); do
	if systemctl is-active --quiet castkms-attach.service &&
		sudo modetest -a -M castkms -c 2>/dev/null |
		awk '$3 == "connected" && $4 ~ /^Virtual-/ { found = 1 }
		     END { exit found ? 0 : 1 }'; then
		printf '%s\n' 'castkms_attach=active'
		exit 0
	fi
	if ! systemctl is-active --quiet castkms-attach.service &&
		test "$attempt" -gt 5; then
		break
	fi
	sleep 0.2
done

printf '%s\n' 'castkms attach service failed to plug a monitor' >&2
systemctl status --no-pager -l castkms-attach.service >&2 || true
journalctl -u castkms-attach.service --no-pager -n 40 >&2 || true
exit 1
