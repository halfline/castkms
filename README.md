# castkms

`castkms` is an experimental, VKMS-derived DRM/KMS driver for virtual display
and passive frame-capture development. It currently provides a fully renamed
VKMS baseline that builds as an external `castkms.ko` module.

The imported source and matching kernel baseline are recorded in
[`UPSTREAM.md`](UPSTREAM.md).

## Default outputs

The default device has one virtual display. Set `max_outputs=N` when
loading the module to create N independent CRTC, encoder, and connector
pipelines. Its feature-dependent limit reserves DRM object-mask slots
for cursor planes, overlays, and writeback; invalid values reject device
creation.

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

## Capture grants

The current top-level Direct Rendering Manager (DRM) owner master may issue a
connector-scoped grant with `DRM_IOCTL_CASTKMS_CREATE_GRANT`. A lease master or
an ordinary primary-node client cannot create one. The returned descriptor is
a fresh, unauthenticated DRM file that cannot become master, may be passed with
`SCM_RIGHTS`, and owns the grant until its final close.

A grant with `MANAGE_ATTACHMENT` may call `ATTACH_MONITOR` for its connector.
The operation changes a disconnected virtual port into a connected monitor,
optionally publishes its EDID, and emits the standard KMS hotplug event. Grant
revocation disconnects an attachment owned by that grant.

Public definitions are in
[`include/uapi/drm/castkms_drm.h`](include/uapi/drm/castkms_drm.h), and
[`docs/capture-grants.md`](docs/capture-grants.md) records the complete grant
and capture contract. Build the userspace grant lifecycle probe with:

```sh
make tools
```

`make check` builds that probe, checks shell and architecture rules, and runs
its command-line smoke test without requiring a loaded device.

## VM smoke test

The repository includes a reproducible QEMU/KVM guest for development on hosts
that cannot load unsigned modules:

```sh
./scripts/vm/castkms-vm provision
./scripts/vm/castkms-vm test
```

See [`docs/vm-testing.md`](docs/vm-testing.md) for lifecycle commands,
configuration, test coverage, and graphical-console setup.

## License

The driver retains the original Linux kernel SPDX declarations and authorship.
See [`COPYING`](COPYING).
