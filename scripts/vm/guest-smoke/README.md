# Guest smoke scenario modules

`modules.sh` is the authoritative source graph used by both the runner and the
host-side structural check. `common.sh` defines the ordered scenario registry,
validates it, and dispatches the requested scenario. Every other shell file
corresponds to one registry entry and defines its `run_*_scenario` entrypoint.
Scenario files may also define private helpers, but they must not execute test
work while being sourced.


To add a scenario:

1. Add `<name>.sh` with one `run_<name>_scenario` function, using underscores
   in the function name when the command-line name contains hyphens.
2. Source the file from `modules.sh`.
3. Add its command-line name to the ordered list and its function to the
   handler map in `common.sh`.
4. Document the scenario in `docs/vm-testing.md` and run
   `make check-smoke-modules check-shell`.

`scripts/check-smoke-modules.sh` rejects malformed, duplicate, or missing
registry entries without booting the VM.
