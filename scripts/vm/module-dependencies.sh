#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only

# insmod deliberately does not resolve either hard or soft module dependencies.
# Load the dependency set recorded in the built object before inserting it.
load_module_dependencies()
{
	local module_path=${1:?missing module path}
	local dependency_csv
	local soft_dependency_line
	local dependency
	local -a dependencies=()

	dependency_csv=$(modinfo -F depends "$module_path")
	if test -n "$dependency_csv"; then
		IFS=',' read -r -a dependencies <<< "$dependency_csv"
	fi

	soft_dependency_line=$(modinfo -F softdep "$module_path")
	for dependency in $soft_dependency_line; do
		case "$dependency" in
		pre:|post:)
			continue
			;;
		esac
		dependencies+=("$dependency")
	done

	if test "${#dependencies[@]}" -gt 0; then
		if test "$EUID" -eq 0; then
			modprobe --all "${dependencies[@]}"
		else
			sudo modprobe --all "${dependencies[@]}"
		fi
	fi
}
