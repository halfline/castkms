# castkms

`castkms` is an experimental DRM/KMS virtual display sink. A capture agent
attaches a monitor, lets an ordinary compositor modeset and render it, captures
the resulting frames with explicit ownership synchronization, and publishes
them to a consumer such as PipeWire. The driver is derived from VKMS, but DRM
test topology, CRC, and writeback are supporting facilities rather than the
primary product path.

The imported source and matching kernel baseline are recorded in
[`UPSTREAM.md`](UPSTREAM.md). The ownership and synchronization invariants are
documented in [`docs/architecture.md`](docs/architecture.md).

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
(`1` by default). Loading the module does not create a monitor. A privileged
agent plugs a sink in with `ATTACH_MONITOR`, optionally supplies an EDID, and
unplugs it with `DETACH_MONITOR` or by closing its DRM file. Standard connector
state, EDID properties, and hotplug events let an unmodified compositor own the
display side.

The experimental capture UAPI is version `0.8`. It provides per-CRTC capability
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

Run the neutral protocol test with:

```sh
sudo ./tools/castkms-capture-test /dev/dri/cardN CRTC-ID
```

Public definitions are in
[`include/uapi/drm/castkms_drm.h`](include/uapi/drm/castkms_drm.h).
Capture and attachment ioctls other than capability queries remain root-only
while the seat/session permission model is being designed.

## PipeWire video

`tools/pw-castkms/pw-castkms` is the reference end-to-end client. It attaches a
monitor, follows mode generations, queues synchronized capture buffers, and
publishes a PipeWire video source:

```sh
sudo ./tools/pw-castkms/pw-castkms -d /dev/dri/cardN -c CRTC-ID
```

The VM smoke test negotiates that node and validates frame pixels, monotonic
sequence/timestamps, and metadata. It is a reference bridge, not yet a complete
desktop session agent or installed user service.

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
part of the default VM or PipeWire product path. Clients must treat it as
development-only until its lifecycle and session ownership are completed.

## VM integration gate

The repository includes a reproducible QEMU/KVM guest for development on hosts
that cannot load unsigned modules:

```sh
./scripts/vm/castkms-vm provision
./scripts/vm/castkms-vm test
```

The smoke command builds and executes KUnit before exercising attachment,
capture, synchronization, modesets, CRC/writeback overlap, PipeWire video,
audio, and teardown. A separate desktop instance checks Mutter discovery:

```sh
./scripts/vm/castkms-vm desktop-provision
./scripts/vm/castkms-vm desktop-test
```

See [`docs/vm-testing.md`](docs/vm-testing.md) for lifecycle commands,
configuration, coverage, and graphical-console setup.

## License

The driver retains the original Linux kernel SPDX declarations and authorship.
See [`COPYING`](COPYING).
