# VM testing

This is the operational guide for the QEMU/KVM test guest. Use it when the
host cannot load an unsigned module, or when you want a clean kernel that
matches the VKMS baseline in `UPSTREAM.md`.

Under the hood it uses a checksum-pinned Fedora 43 cloud image, a qcow2
overlay, a dedicated local SSH key, QEMU user networking, and the Fedora
`7.2.0-61.fc45.x86_64` kernel built from the source baseline in
`UPSTREAM.md`. QEMU boots with ordinary BIOS firmware, so the guest does not
enforce Secure Boot module signatures, which is what lets it load the
unsigned module. Userspace packages are resolved from the Fedora repositories
when an overlay is provisioned; every run records the exact installed NEVRAs
in `guest-packages.txt`, plus the smaller desktop package manifest when
applicable.

## Quick start

Provision the guest and run the smoke test:

```sh
./scripts/vm/castkms-vm provision
./scripts/vm/castkms-vm test
./scripts/vm/castkms-vm kunit-test
```

`provision` downloads the base image, creates a 30 GiB sparse overlay, boots
the guest, installs the pinned kernel and build tools, and reboots the guest
into that kernel. Re-running it is safe and idempotent.

`test` mirrors the current working tree into the guest and then:

1. builds `castkms.ko` and the four-suite KUnit module with `W=1`;
2. verifies both modules' names, vermagic, dependencies, legacy strings, and
   exported symbols;
3. loads stock `vkms` and `castkms` together without default devices;
4. verifies independent `vkms` and `castkms` configfs roots;
5. creates a device through configfs and verifies topology removal safely
   disables and unplugs it before detaching configuration, including explicit
   ioctl and debugfs failures through file descriptors kept open across
   removal;
6. creates a default `castkms` DRM card with a color pipeline and writeback
   connector;
7. performs a bounded preferred-mode, vsynced page-flip test;
8. keeps CRC capture open across two writeback jobs, verifies both fences and
   output buffers, and requires fresh CRC records after writeback cleanup;
9. records `modetest`, `drm_info`, CRC, writeback, and lifecycle output;
10. unloads every module it loaded and verifies cleanup.

`kunit-test` rebuilds all four audio/CEC inclusion combinations, builds the
KUnit module with `W=1`, loads the Fedora KUnit support and CastKMS modules,
and requires all nine CastKMS suites to pass. It also rejects kernel warnings
and diagnostics, unloads both project modules, and copies its build log,
kernel log, provenance, package manifests, and summary into the default result
directory. The standalone `make kunit` target remains available for build-only
coverage on a host that cannot load the modules.

Results are copied to:

```text
~/.cache/castkms-vm/results/default/
```

Useful commands:

```sh
./scripts/vm/castkms-vm status
./scripts/vm/castkms-vm shell
./scripts/vm/castkms-vm logs
./scripts/vm/castkms-vm sync
./scripts/vm/castkms-vm stop
```

The default SSH forward is `127.0.0.1:22222`. Override it when running more
than one instance:

```sh
CASTKMS_VM_INSTANCE=second \
CASTKMS_VM_SSH_PORT=22223 \
./scripts/vm/castkms-vm provision
```

Tests that need discovery from a host network can add a second guest NIC while
retaining the user-network NIC used for SSH:

```sh
CASTKMS_VM_LAN_BRIDGE=virbr0 ./scripts/vm/castkms-vm start
```

The named host bridge must already exist and be allowed by QEMU's bridge-helper
policy. On a Wi-Fi host, use a routed or NATed bridge and reflect only the
required multicast-DNS services between that bridge and the Wi-Fi interface;
an ordinary Linux bridge cannot transparently carry a guest's second MAC over
most station-mode Wi-Fi connections. This option is deliberately opt-in so
kernel-only VM runs keep their isolated user network.

`reset` stops the guest and moves its instance directory into the state
directory's `archive/` folder before creating a fresh overlay. The downloaded
base image and SSH key are retained.

Each VM test copies its logs to
`~/.cache/castkms-vm/results/default/`. The fast test includes its KUnit,
live-grant, and kernel logs. After the product test has stopped its processes,
closed its files, and unloaded the modules, it saves the complete kernel log as
`product-dmesg.txt`. It fails if that log contains a kernel warning or bug, a
report of invalid memory use or undefined behavior, a reference-count failure,
a report that locks may be taken in an inconsistent order, or a DRM error.
When that check fails, the matching lines are also saved in
`product-kernel-errors.txt`. `kernel-debug-features.txt` records which relevant
instrumentation the running kernel actually enables. The pinned Fedora kernel
enables UBSAN but not KASAN, KCSAN, or lockdep, so those stronger diagnostics
require a separately built debug kernel rather than being implied by the log
pattern gate.

On a host that can load the modules, `make kunit` builds the test module
directly.

## Product scenarios

`test` performs module identification and teardown before running the selected
product work.

The guest harness keeps lifecycle setup and the single failure-safe cleanup
trap in `guest-smoke-test.sh`. Its product scenarios live in
`scripts/vm/guest-smoke/`, with an explicit ordered registry in `common.sh`.
`make check-smoke-modules` checks the registry and source layout without
starting the VM; the module contract is documented in that directory's
`README.md`.


## Graphical testing

The VM includes a virtio VGA device. Set `CASTKMS_VM_VNC_DISPLAY` before
starting it to expose a local-only VNC console; display `9` listens on TCP
port `5909`:

```sh
./scripts/vm/castkms-vm stop
CASTKMS_VM_VNC_DISPLAY=9 ./scripts/vm/castkms-vm start
```

GNOME/Mutter testing uses a separate `desktop` instance so the default
kernel-smoke guest stays minimal:

```sh
./scripts/vm/castkms-vm desktop-provision
./scripts/vm/castkms-vm desktop-test
```

Those commands default to instance `desktop`, SSH port `22223`, 8 GiB of
guest memory, and VNC display `9` (`127.0.0.1:5909`). Override them with the
same `CASTKMS_VM_*` variables used by the headless instance.

`desktop-provision` runs the ordinary kernel and toolchain install, then adds
GNOME Shell, Mutter, GDM, and Mesa GBM/software renderers, enables the
graphical target, and configures passwordless GDM autologin for the `castkms`
user. After reboot it synchronizes and builds the current tree, installs the
attachment service outside the home directory, and leaves `VirtualScreen`
attached. `desktop-start` boots an already provisioned desktop guest and
refreshes that runtime, while `desktop-attach` performs only the refresh on a
guest that is already running.

`desktop-test` loads `castkms` with a default disconnected virtual connector
and no extra planes or writeback, checks that the card's udev properties do
not carry `mutter-device-ignore`, restarts GDM so the session enumerates the
new KMS device, and waits for `org.gnome.Shell` and
`org.gnome.Mutter.DisplayConfig`. `GetCurrentState` must not list the Virtual
connector until the attachment service runs; afterward, the connector must
appear.

Results are copied to `~/.cache/castkms-vm/results/desktop/`. Connect a VNC
client to port `5909` to inspect the running GNOME session after a successful
test.

Useful desktop lifecycle commands:

```sh
./scripts/vm/castkms-vm desktop-status
./scripts/vm/castkms-vm desktop-start
./scripts/vm/castkms-vm desktop-attach
./scripts/vm/castkms-vm desktop-shell
./scripts/vm/castkms-vm desktop-stop
```
