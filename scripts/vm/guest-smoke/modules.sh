# SPDX-License-Identifier: GPL-2.0-only
# shellcheck shell=bash
#
# Authoritative source graph for the guest smoke harness.

_smoke_module_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

# shellcheck source=common.sh
. "$_smoke_module_dir/common.sh"

unset _smoke_module_dir
