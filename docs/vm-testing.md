# VM testing

The VM harness provides a disposable environment for building and loading the
driver without changing the host's boot or module-signing policy.

It uses a checksum-pinned Fedora 43 cloud image, a qcow2 overlay, a dedicated
local SSH key, QEMU user networking, and the Fedora `7.1.7-100.fc43.x86_64` kernel
that matches the source baseline in `UPSTREAM.md`. QEMU uses its ordinary BIOS
firmware, so the guest does not enforce Secure Boot module signatures.

## Quick start

Provision the guest and run the smoke test:

```sh
./scripts/vm/castkms-vm provision
./scripts/vm/castkms-vm test
```

`provision` downloads the base image, creates a 30 GiB sparse overlay, boots
the guest, installs the pinned kernel and build tools, and reboots the guest
into that kernel. Re-running it is safe and idempotent.

`test` mirrors the current working tree into the guest and then:

1. builds `castkms.ko` with `W=1`;
2. verifies the module name, vermagic, legacy strings, and exported symbols;
3. loads stock `vkms` and `castkms` together without default devices;
4. verifies independent `vkms` and `castkms` configfs roots;
5. creates a default `castkms` DRM card with optional planes disabled;
6. performs a bounded preferred-mode, vsynced page-flip test;
7. records `modetest` and `drm_info` output;
8. unloads every module it loaded and verifies cleanup.

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

The base cloud image is intentionally minimal. GNOME/Mutter installation and
interactive display testing are a separate step from the kernel smoke test.
