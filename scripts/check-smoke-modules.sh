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

for scenario in "${smoke_scenario_names[@]}"; do
	smoke_validate_scenario "$scenario"
	if ! test -r "$module_dir/$scenario.sh"; then
		printf 'missing smoke scenario module: %s.sh\n' "$scenario" >&2
		exit 1
	fi
done

if smoke_validate_scenario invalid-scenario >/dev/null 2>&1; then
	printf '%s\n' 'invalid smoke scenario unexpectedly passed validation' >&2
	exit 1
fi

printf 'smoke-modules=pass (%s scenarios)\n' \
	"${#smoke_scenario_names[@]}"
