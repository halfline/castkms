# castkms

`castkms` is an experimental DRM/KMS virtual display sink. A capture agent
attaches a monitor, lets an ordinary compositor modeset and render it, captures
the resulting frames with explicit ownership synchronization, and publishes
them to a consumer such as PipeWire. The driver is derived from VKMS, but DRM
test topology, CRC, and writeback are supporting facilities rather than the
primary product path.

The imported source and matching kernel baseline are recorded in
[`UPSTREAM.md`](UPSTREAM.md). The ownership and synchronization invariants are
documented in [`docs/architecture.md`](docs/architecture.md). The capture
authorization contract is documented in
[`docs/capture-grants.md`](docs/capture-grants.md).

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

Build the protocol tests and PipeWire bridge with:

```sh
make tools
```

## Monitor attachment and capture

The default device publishes `max_outputs` disconnected virtual connectors
(`1` by default). Loading the module does not create a monitor. The top-level
DRM owner master can create a connector-scoped normal grant directly.
Initial-user-namespace root can create either a holder-lived delegated grant
bound to the current owner master or an explicitly roaming administrative
grant. DRM lease masters cannot create normal grants. Grant rights control
monitor attachment, EDID, pixels, cursor data, and CEC independently. Opening
the card—even as a DRI-enabled sandbox—does not grant any of those operations.

The experimental capture UAPI is version `0.9`. It provides per-CRTC capability
queries, exclusive stream ownership, up to eight persistent destinations, one
in-flight capture plus one queued destination, implicit reservation fences, and
explicit timeline syncobjs. Completion events carry sequence, timestamp, mode
generation, damage, and cursor metadata, but the producer fence is always the
buffer ownership boundary.

Destinations are currently linear `XRGB8888` framebuffers backed by GEM objects
created on the castkms device. They may be exported to consumers as DMA-BUFs;
foreign DMA-BUF import is not supported and is reported by the absence of
`DRM_CASTKMS_CAPTURE_CAP_DMA_BUF_IMPORT`. A mode change invalidates the stream
generation, so the agent must stop, restart, and register mode-sized buffers.
Version 0 interfaces may still change.

Run the focused grant authority/lifetime test with a connector object ID:

```sh
sudo ./tools/castkms-grant-test /dev/dri/cardN CONNECTOR-ID
```

Public definitions are in
[`include/uapi/drm/castkms_drm.h`](include/uapi/drm/castkms_drm.h). Sensitive
operations are no longer root-only: they require a matching active grant.
Grant creation accepts top-level owner-master authority or an explicit root
mode. A one-shot root helper can create a delegated grant bound to the current
owner, pass its fd, and exit without revoking it. The grant suspends across
other masters and revivifies only for that exact owner identity. An explicit
root administrative grant instead follows capture-safe content across master
handoffs and remains tied to its creator file as a revocation anchor.

The broader `castkms-capture-test` still describes the pre-`0.9` single-fd
workflow. It is retained as a userspace migration fixture, but must be taught
to receive a grant fd before its sensitive-operation cases can run against
this UAPI.

## PipeWire video

`tools/pw-castkms/pw-castkms` is the pre-grant reference bridge. Its capture,
mode-generation, and PipeWire code remains useful, but it does not yet accept
an inherited grant fd and therefore cannot use the `0.9` sensitive operations.
That migration and PipeWire node-access policy are deliberately separate from
the kernel grant implementation; root is not an operational bypass.

## HDMI audio

Audio is enabled by default with `enable_audio=1`. Each output behaves as an
HDMI PCM sink with connector jack state, ELD derived from the attached EDID,
pause/resume, and ALSA timing. The kernel PCM endpoint models presentation and
does not retain an independent copy of samples for the capture UAPI. A local
remote-desktop agent can capture audio from the PipeWire sink monitor; there is
no capture-associated kernel audio transport today.

## CEC

CEC is disabled by default and remains a separate experimental `0.1` transport.
Enable it explicitly with `enable_cec=1`. The current capability query does not
advertise state events, only one transmit may be outstanding, and CEC is not
part of the default VM or PipeWire product path. Its transport now follows the
grant's CEC right and revoke/suspend lifetime, but end-to-end session coverage
is still development-only.

## VM integration gate

The repository includes a reproducible QEMU/KVM guest for development on hosts
that cannot load unsigned modules:

```sh
./scripts/vm/castkms-vm provision
./scripts/vm/castkms-vm kunit-test
```

The KUnit command builds and executes all six suites, runs the focused live-fd
grant lifecycle test with CEC enabled, rejects kernel warnings, and verifies a
clean unload. The broader `test` command still contains pre-`0.9` capture and
PipeWire clients and will become the full gate again after their inherited-
grant migration. A separate desktop instance checks Mutter discovery:

```sh
./scripts/vm/castkms-vm desktop-provision
./scripts/vm/castkms-vm desktop-test
```

See [`docs/vm-testing.md`](docs/vm-testing.md) for lifecycle commands,
configuration, coverage, and graphical-console setup.

## License

The driver retains the original Linux kernel SPDX declarations and authorship.
See [`COPYING`](COPYING).
