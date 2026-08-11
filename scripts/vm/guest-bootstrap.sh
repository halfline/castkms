#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

target_release=${1:?missing target kernel release}
rpm_base_url=${2:?missing kernel RPM base URL}
rpm_dir=/var/tmp/castkms-kernel-$target_release
toolchain_stamp=/var/lib/castkms-vm/toolchain-v1
kernel_ready=1

if test ! -e "$toolchain_stamp"; then
	sudo dnf -y -q --setopt=install_weak_deps=False install \
		bc \
		bison \
		curl \
		drm_info \
		drm-utils \
		elfutils-libelf-devel \
		flex \
		gcc \
		kmod \
		make \
		openssl-devel \
		perl-interpreter \
		rsync
	sudo mkdir -p "$(dirname -- "$toolchain_stamp")"
	sudo touch "$toolchain_stamp"
fi

for package in kernel kernel-core kernel-modules-core kernel-modules kernel-devel; do
	if ! rpm -q "$package-$target_release" >/dev/null 2>&1; then
		kernel_ready=0
	fi
done

if test "$kernel_ready" -eq 0; then
	mkdir -p "$rpm_dir"
	for package in kernel kernel-core kernel-modules-core kernel-modules kernel-devel; do
		rpm_name=$package-$target_release.rpm
		if test ! -f "$rpm_dir/$rpm_name"; then
			curl --fail --location --show-error --continue-at - \
				--output "$rpm_dir/$rpm_name" "$rpm_base_url/$rpm_name"
		fi
	done

	rpmkeys --checksig "$rpm_dir"/*.rpm
	sudo dnf -y -q install "$rpm_dir"/*.rpm
fi

test -f "/boot/vmlinuz-$target_release"
sudo grubby --set-default "/boot/vmlinuz-$target_release"

printf 'installed_kernel=%s\n' "$target_release"
printf 'running_kernel=%s\n' "$(uname -r)"
printf 'default_kernel=%s\n' "$(sudo grubby --default-kernel)"
