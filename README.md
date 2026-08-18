# castkms

`castkms` is an experimental, VKMS-derived DRM/KMS driver for virtual display
and passive frame-capture development. It currently provides a fully renamed
VKMS baseline that builds as an external `castkms.ko` module.

The imported source and matching kernel baseline are recorded in
[`UPSTREAM.md`](UPSTREAM.md).

## Build

The default build uses the development tree for the running kernel:

```sh
make W=1
modinfo ./castkms.ko
```

Override `KDIR` to target another fully prepared kernel build:

```sh
make KDIR=/path/to/kernel/build W=1
```

## Experimental capture protocol

The experimental version 0.5 capture interface provides a per-CRTC capability
query, exclusive stream ownership, persistent destination-buffer registration,
and bounded frame delivery. It accepts up to eight linear `XRGB8888`
framebuffers per stream, keeps at most one capture in flight, and can queue the
next destination for a future vblank. Completion arrives as a reserved DRM
event with the vblank sequence, monotonic timestamp, mode generation, and
full-frame damage. Implicit queueing honors existing reservation fences without
waiting in the vblank path and attaches a producer fence before returning.
Explicit queueing
waits for a consumer-owned reuse timeline point and publishes the producer
fence at a new ready timeline point; the first queue may use reuse point zero,
and subsequent points must advance. Events report completion metadata but do
not transfer buffer ownership; the implicit reservation fence or explicit
ready point remains the ownership boundary. Capture destinations currently
must be GEM
objects created by castkms and may be exported as DMA-BUFs for consumers. Each
explicit buffer uses a dedicated ready/reuse syncobj pair retained at
registration.

Starting a stream observes a CRTC without activating or modesetting it.
After a mode change, stop that generation-bound stream and start a new one
before registering or queueing buffers for the new mode.
Stopping the stream or closing its DRM file cancels pending work and unmaps and
releases every registered buffer automatically. Version 0 interfaces may
change as the remaining operations are developed.

Build the neutral userspace protocol test with:

```sh
make tools
sudo ./tools/castkms-capture-test /dev/dri/cardN CRTC-ID
```

The public definitions are in
[`include/uapi/drm/castkms_drm.h`](include/uapi/drm/castkms_drm.h).

## VM smoke test

The repository includes a reproducible QEMU/KVM guest for development on hosts
that cannot load unsigned modules:

```sh
./scripts/vm/castkms-vm provision
./scripts/vm/castkms-vm test
```

A separate desktop instance installs GNOME/Mutter and checks that the virtual
connector is visible to the compositor:

```sh
./scripts/vm/castkms-vm desktop-provision
./scripts/vm/castkms-vm desktop-test
```

See [`docs/vm-testing.md`](docs/vm-testing.md) for lifecycle commands,
configuration, test coverage, and graphical-console setup.

## License

The driver retains the original Linux kernel SPDX declarations and authorship.
See [`COPYING`](COPYING).
