# VM testing

The VM harness provides a disposable environment for building and loading the
driver without changing the host's boot or module-signing policy.

It uses a checksum-pinned Fedora 43 cloud image, a qcow2 overlay, a dedicated
local SSH key, QEMU user networking, and the Fedora `7.1.7-100.fc43.x86_64` kernel
that matches the source baseline in `UPSTREAM.md`. QEMU uses its ordinary BIOS
firmware, so the guest does not enforce Secure Boot module signatures.

## Quick start

Provision the guest and run the kernel test gate:

```sh
./scripts/vm/castkms-vm provision
./scripts/vm/castkms-vm kunit-test
```

`provision` downloads the base image, creates a 30 GiB sparse overlay, boots
the guest, installs the pinned kernel and build tools, and reboots the guest
into that kernel. Re-running it is safe and idempotent.

`kunit-test` mirrors the current working tree into the guest, builds with
`W=1`, runs all six suites, then loads the normal device with CEC enabled and
runs the focused live grant-fd lifecycle gate. It rejects kernel warnings and
requires a clean module unload.

The broader `test` command currently retains the pre-`0.9` single-fd capture
and PipeWire workflow. Its post-KUnit phases require userspace support for an
inherited grant fd and are not expected to pass during that migration. Once
updated, those phases cover the following existing integration matrix:

1. builds `castkms.ko` and the six-suite KUnit module with `W=1`, executes all
   KUnit suites, and builds the userspace protocol and PipeWire tests;
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
7. verifies syncobj and timeline-syncobj DRM capabilities, drops DRM master,
   performs the capture format query's count and data calls, requires linear
   `XRGB8888`, and verifies exclusive stream ownership; publishes a valid
   output EDID onto the display connector, rejects invalid blobs, foreign
   stream owners, and EDID updates before attach; keeps that EDID across
   stream stop; and clears it on detach and file close; registers mapped
   buffers in implicit and explicit synchronization modes; rejects stale
   generations, wrong dimensions, invalid timeline points, shared syncobj
   ownership, and foreign ownership; and verifies buffer cleanup on unregister,
   stream stop, and file close; queues an implicit destination for a future
   vblank, exports its DMA-BUF reservation fence, imports that fence as a reader
   of a second destination, and records whether scheduling left the dependency
   pending when the second buffer was queued; repeats the reuse sequence with
   explicit ready and reuse syncobj timeline points; verifies that all producer
   fences signal in sequence, then checks bounded dropped-frame metadata,
   completion events, full damage metadata, and composed pixels;
8. performs a bounded `800x600`, vsynced page-flip test and verifies that its
   modeset advances the capture mode generation;
9. keeps CRC capture open, queues one capture destination while a writeback
   job is composing, verifies the capture event, producer fence, and composed
   pixels, then runs two further writeback jobs, checks both fences and output
   buffers, and requires fresh CRC records after writeback cleanup;
10. records `modetest`, `drm_info`, capture, CRC, writeback, and lifecycle
    output;
11. unloads every module it loaded and verifies cleanup.

The full smoke test invokes `guest-kunit-test.sh`, loads `kunit`, `castkms`, and the
out-of-tree test module, and rejects missing or failing suites before continuing
with integration coverage. Run that gate alone with
`./scripts/vm/castkms-vm kunit-test`; the standalone build target remains
available as `make kunit`.

The grant-specific kernel/UAPI tool can also be run independently after
loading the module:

```sh
sudo ./tools/castkms-grant-test /dev/dri/cardN CONNECTOR-ID
```

It verifies denial on an ordinary fd, grant creation by DRM master, fd passing,
positive and missing-right attachment checks, and real modeset plus
`START`/`REGISTER_BUFFER`/`QUEUE_BUFFER` frame capture. It stress-tests a new
stream across 32 immediate drop/reacquire cycles to cover deferred master
cleanup. A one-shot privileged non-master then creates a delegated grant bound
to the current owner and exits; the test verifies that the holder remains
usable, its old stream is collected on master loss, another master cannot
activate it, and the original master can revivify it after presenting safe
content. It also checks root revocation and final-holder cleanup after creator
exit. A replacement master is denied capture, CRC, and writeback access to the
first master's residual composition; after presenting its own framebuffer,
its normal grant and the roaming administrative grant can capture. The test
also covers explicit normal revocation and creator-close revocation for the
administrative class.

KUnit logs, live-grant output, and the live kernel log are copied to:

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

`reset` stops the guest and moves its instance directory into the state
directory's `archive/` folder before creating a fresh overlay. The downloaded
base image and SSH key are retained.

## Graphical testing

The VM includes a virtio VGA device. Set `CASTKMS_VM_VNC_DISPLAY` before
starting it to expose a local-only VNC console; display `9` listens on TCP
port `5909`:

```sh
./scripts/vm/castkms-vm stop
CASTKMS_VM_VNC_DISPLAY=9 ./scripts/vm/castkms-vm start
```

## Mutter visibility

The kernel smoke test stays on the minimal cloud image. Mutter visibility uses
the same harness, base image, SSH key, and pinned kernel, but a separate
`desktop` instance so GNOME packages never land on the default guest.

```sh
./scripts/vm/castkms-vm desktop-provision
./scripts/vm/castkms-vm desktop-test
```

Those commands default to instance `desktop`, SSH port `22223`, 8 GiB of
guest memory, and VNC display `9` (`127.0.0.1:5909`). Override them with the
same `CASTKMS_VM_*` variables used by the headless instance.

`desktop-provision` runs the ordinary kernel/toolchain install, then adds
GNOME Shell, Mutter, GDM, Mesa GBM/software renderers, enables the graphical
target, and configures passwordless GDM autologin for the `castkms` user.
It then syncs the tree, builds the module and capture tool, and starts a
guest systemd unit that holds `ATTACH_MONITOR` so Settings sees
`VirtualScreen` without a manual attach. A later guest reboot restarts that
unit when `castkms.ko` is still on disk.

`desktop-start` and `desktop-attach` perform the same sync/build/attach
step on an already-provisioned desktop guest.

`desktop-test` then:

1. builds and loads `castkms` with a default disconnected virtual connector
   and no extra planes or writeback;
2. requires the card's udev properties not to carry `mutter-device-ignore`
   (Mutter's stock rules match only `platform-vkms` and `/devices/faux/vkms/`);
3. restarts GDM so the session enumerates the new KMS device;
4. waits for `org.gnome.Shell` and `org.gnome.Mutter.DisplayConfig`;
5. requires `GetCurrentState` not to list the Virtual connector until
   `ATTACH_MONITOR`, then attaches a monitor and requires the connector to
   appear.

Results are copied to `~/.cache/castkms-vm/results/desktop/`. Use
`./scripts/vm/castkms-vm desktop-shell` and a VNC client on port `5909` to
inspect GNOME Settings by hand.

```sh
./scripts/vm/castkms-vm desktop-status
./scripts/vm/castkms-vm desktop-start
./scripts/vm/castkms-vm desktop-attach
./scripts/vm/castkms-vm desktop-shell
./scripts/vm/castkms-vm desktop-stop
```
