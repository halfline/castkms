# SPDX-License-Identifier: GPL-2.0-only
# shellcheck shell=bash
#
# Sourced by modules.sh.
# shellcheck disable=SC2154

declare -ar smoke_scenario_names=(
	configfs
	capture
	cursor
	pipewire-audio
)
declare -Ar smoke_scenario_handlers=(
	[configfs]=run_configfs_scenario
	[capture]=run_capture_scenario
	[cursor]=run_cursor_scenario
	[pipewire-audio]=run_pipewire_audio_scenario
)

smoke_validate_scenario()
{
	local candidate=$1
	local name

	if test "$candidate" = all; then
		return 0
	fi
	for name in "${smoke_scenario_names[@]}"; do
		if test "$candidate" = "$name"; then
			return 0
		fi
	done

	printf 'unknown smoke scenario: %s\n' "$candidate" >&2
	return 2
}

smoke_validate_registry()
{
	local -A seen_handlers=()
	local -A seen_names=()
	local handler
	local index
	local name

	if test "${#smoke_scenario_names[@]}" -ne \
		"${#smoke_scenario_handlers[@]}"; then
		printf '%s\n' 'smoke scenario registry has mismatched names and handlers' >&2
		return 1
	fi
	if test "${#smoke_scenario_names[@]}" -eq 0; then
		printf '%s\n' 'smoke scenario registry is empty' >&2
		return 1
	fi

	for ((index = 0; index < ${#smoke_scenario_names[@]}; index++)); do
		name=${smoke_scenario_names[$index]}
		if [[ ! $name =~ ^[a-z0-9]+(-[a-z0-9]+)*$ ]]; then
			printf 'invalid smoke scenario name: %s\n' "$name" >&2
			return 1
		fi
		if test -n "${seen_names[$name]+present}"; then
			printf 'duplicate smoke scenario name: %s\n' "$name" >&2
			return 1
		fi
		if test -z "${smoke_scenario_handlers[$name]+present}"; then
			printf 'missing smoke scenario handler mapping: %s\n' "$name" >&2
			return 1
		fi
		handler=${smoke_scenario_handlers[$name]}
		if test -n "${seen_handlers[$handler]+present}"; then
			printf 'duplicate smoke scenario handler: %s\n' "$handler" >&2
			return 1
		fi
		seen_names[$name]=1
		seen_handlers[$handler]=1

		if ! [[ $handler =~ ^run_[a-z0-9_]+_scenario$ ]]; then
			printf 'invalid smoke scenario handler: %s\n' "$handler" >&2
			return 1
		fi
		if ! declare -F "$handler" >/dev/null; then
			printf 'missing smoke scenario handler: %s\n' "$handler" >&2
			return 1
		fi
	done
}

scenario_begin()
{
	printf 'scenario_%s=start\n' "$1" | tee -a "$result_dir/summary.txt"
}

scenario_end()
{
	printf 'scenario_%s=pass\n' "$1" | tee -a "$result_dir/summary.txt"
}

smoke_run_selected_scenarios()
{
	local name
	local selected=$1

	for name in "${smoke_scenario_names[@]}"; do
		if test "$selected" = all ||
			test "$selected" = "$name"; then
			"${smoke_scenario_handlers[$name]}"
		fi
	done
}
