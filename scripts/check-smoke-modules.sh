#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
module_dir=$script_dir/vm/guest-smoke

# Load the exact module graph used by the guest runner.
# shellcheck source=vm/guest-smoke/modules.sh
. "$module_dir/modules.sh"

smoke_validate_registry
smoke_validate_scenario all

declare -A registered_modules=()
for scenario in "${smoke_scenario_names[@]}"; do
	smoke_validate_scenario "$scenario"
	if ! test -r "$module_dir/$scenario.sh"; then
		printf 'missing smoke scenario module: %s.sh\n' "$scenario" >&2
		exit 1
	fi
	registered_modules[$scenario]=1
done

for module in "$module_dir"/*.sh; do
	module_name=$(basename -- "$module" .sh)
	case "$module_name" in
	common|modules) continue ;;
	esac
	if test -z "${registered_modules[$module_name]+present}"; then
		printf 'unregistered smoke scenario module: %s\n' "$module" >&2
		exit 1
	fi
done

if smoke_validate_scenario invalid-scenario >/dev/null 2>&1; then
	printf '%s\n' 'invalid smoke scenario unexpectedly passed validation' >&2
	exit 1
fi

printf 'smoke-modules=pass (%s scenarios)\n' \
	"${#smoke_scenario_names[@]}"
