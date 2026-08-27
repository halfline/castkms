# SPDX-License-Identifier: GPL-2.0-only
# shellcheck shell=bash
#
# Authoritative source graph for the guest smoke harness.

_smoke_module_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

# shellcheck source=common.sh
. "$_smoke_module_dir/common.sh"
# shellcheck source=configfs.sh
. "$_smoke_module_dir/configfs.sh"
# shellcheck source=capture.sh
. "$_smoke_module_dir/capture.sh"
# shellcheck source=cursor.sh
. "$_smoke_module_dir/cursor.sh"

unset _smoke_module_dir
