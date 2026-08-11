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
