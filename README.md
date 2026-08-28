# castkms

CastKMS is an experimental virtual monitor for Linux. A compositor such as
Mutter sees it as a normal display output. A capture agent, typically a
remote-desktop or streaming service, plugs that monitor in, the compositor
picks a resolution and refresh rate and draws frames, and the agent copies
those completed frames out. It can then publish them to PipeWire, the
desktop's audio and video plumbing, or to another local consumer.

Opening `/dev/dri/cardN` is not permission to see those pixels, even for a
sandbox with DRI device access. The compositor, or a privileged helper acting
for it, issues a **grant**: a file descriptor that authorizes attachment,
identification, capture, cursor data, and HDMI remote-control for one
connector. Pass that fd to the capture agent.

Grant creation also returns a grantor fd. The compositor keeps that descriptor
private: closing it revokes the holder, and polling it reports `POLLHUP` when
the holder or any other terminal event ends the grant.

The VKMS baseline is recorded in [`UPSTREAM.md`](UPSTREAM.md). People writing
a compositor or capture agent should read
[`docs/capture-grants.md`](docs/capture-grants.md). People changing the driver
should read [`docs/architecture.md`](docs/architecture.md).

## How a session works

1. Load the module. The card appears with `max_outputs` disconnected virtual
   connectors (`8` by default). The upper bound is feature-dependent because
   cursor planes, overlays, and writeback consume additional DRM object-mask
   slots; invalid combinations are rejected before device creation. Nothing is
   plugged in yet.
2. A compositor opens the card and becomes **DRM master**, the process
   allowed to **modeset**, that is, to pick a resolution and refresh rate and
   turn the output on.
3. That compositor creates a grant for a connector, retains the grantor fd,
   and passes the holder fd to the capture agent. For lab work,
   `castkms-grant-launch` can issue an
   administrative grant instead; a production compositor should issue a
   **normal** grant bound to itself. See
   [`docs/capture-grants.md`](docs/capture-grants.md) for the delegated and
   administrative forms.
4. The agent attaches a monitor and publishes an **EDID**, the identification
   blob a real monitor uses to advertise its name, preferred modes, and audio
   capabilities.
5. The compositor notices the hotplug, modesets, and renders.
6. The agent starts a capture stream, queues buffers, and copies completed
   frames to PipeWire or another consumer.

Rights on a grant independently cover monitor attachment, EDID, pixels, cursor
data, and HDMI-CEC. A DRM **lease** (a client given only a subset of the
card's resources) cannot create grants.

The capture protocol is experimental version `0.10` (read as major.minor;
major `0` means it may still change incompatibly). Each stream carries a
single mode's frames: after a modeset the agent stops the stream, starts a
new one, and registers fresh buffers for the new mode. Those buffers are
plain `XRGB8888` images allocated on the grant fd, and can be shared with
another process as a **DMA-BUF**, a buffer passed around as a file
descriptor. Importing a buffer from a different device is not supported.

Public definitions are in
[`include/uapi/drm/castkms_drm.h`](include/uapi/drm/castkms_drm.h). Frame,
cursor, and EDID protocol ceilings are the `DRM_CASTKMS_CAPTURE_MAX_*`
constants in that header. The bundled EDID generator is narrower than the
kernel: it emits a base block plus one extension, while the protocol accepts
up to 512 bytes.

## Build

Build against the running kernel's development tree:

```sh
make W=1
modinfo ./castkms.ko
```

Target another prepared kernel with `KDIR`:

```sh
make KDIR=/path/to/kernel/build W=1
```

HDMI audio and HDMI-CEC compile in when the kernel provides their
dependencies. A package that wants neither can omit them:

```sh
make CASTKMS_BUILD_AUDIO=n CASTKMS_BUILD_CEC=n W=1
```

`make build-matrix W=1` builds all four inclusion combinations plus the
fallback where both kernel options are disabled. The enabled cases need a
kernel with `CONFIG_SND` and `CONFIG_DRM_DISPLAY_HDMI_CEC_HELPER`, and the
HDMI CEC helper header must be present.

Userspace protocol tests and the PipeWire example live under `tools/`:

```sh
make tools
```

`make check` is the host-side gate that does not need a loaded device. It
builds every protocol, audio, and PipeWire client available on the host, runs
the EDID unit tests, checks driver layering and ioctl allowlists, and
executes every available CLI help entry point. Device-backed behavior is
covered by the VM tests below.

## Trying it

On a machine that can load the module:

```sh
sudo insmod ./castkms.ko
sudo ./tools/castkms-grant-test /dev/dri/cardN CONNECTOR-ID
```

`castkms-grant-test` checks that an ordinary card fd cannot capture, that a
master can issue a grant, and that the holder can attach and capture frames.

`castkms-grant-launch` creates an administrative grant and passes it to a
child. Use it in the VM and in the lab; a production compositor should issue a
normal grant instead:

```sh
sudo ./tools/castkms-grant-launch /dev/dri/cardN CONNECTOR-ID -- \
  ./tools/castkms-capture-test /dev/dri/cardN CRTC-ID
```

CastKMS is derived from VKMS, the kernel's virtual KMS test driver. A few
VKMS-style test features are available but off by default:

| Parameter | What it enables |
|---|---|
| `enable_configfs=1` | Build custom display topologies from userspace. |
| `enable_crc=1` | Publish a checksum of each composed frame. |
| `enable_writeback=1` | Copy composed pixels through the standard DRM writeback connector. |
| `enable_fbdev=1` | Attach the in-kernel framebuffer client (off by default, see below). |

Writeback is not capture. Capture is how a grant holder reads frames (the
session above). Writeback is how the DRM master copies that same composed
frame into a framebuffer through the ordinary KMS writeback connector. Leave
writeback off unless a test needs it.

Leave `enable_fbdev` off. Otherwise the in-kernel framebuffer client can take
over the card as DRM master and become the owner of on-screen content.

## PipeWire video

`tools/pw-castkms/pw-castkms` is a small example consumer. It takes a `0.10`
grant through `--grant-fd` or `CASTKMS_GRANT_FD`, creates destination buffers
on that fd, and publishes a PipeWire source. It stops when the grant or mode
generation changes so a supervisor can restart it. Publication uses a
restricted PipeWire connection (`--pipewire-fd` or `PIPEWIRE_REMOTE_FD`);
`--allow-unrestricted-pipewire` is an escape hatch for an isolated development
daemon.

The kernel grant controls who may read pixels. The restricted PipeWire
connection controls who may consume the published node. See the
[`pw-castkms` reading guide](tools/pw-castkms/README.md) for the protocol and
buffer-lifetime walkthrough.

## HDMI audio

If the module was built with audio, it is enabled by default
(`enable_audio=1`). Each output looks like an HDMI PCM sink to ALSA: jack
detection follows connector plug, and **ELD** (the short audio-capability list
players read) is derived from the attached EDID. Pause, resume, and
presentation timing work as they would on a real HDMI device. The kernel
models that presentation; it does not keep a second copy of samples for the
capture API. A local agent can capture audio from the PipeWire sink's monitor
instead.

## HDMI-CEC

HDMI **CEC** (Consumer Electronics Control) is the low-speed command channel
HDMI devices use for things like power, volume, and input switching. If the
module was built with CEC, it is enabled by default. Disable it with
`enable_cec=0`.

The experimental `0.1` transport follows the grant's CEC right and the grant's
revoke and suspend lifetime. Only one transmit may be outstanding, and the
capability query does not advertise state events. `castkms-cec-test` consumes
`CASTKMS_GRANT_FD`; the lab launcher grants the CEC right.

## VM tests

On hosts that cannot load unsigned modules, a QEMU/KVM guest builds and
exercises the driver:

```sh
./scripts/vm/castkms-vm provision
./scripts/vm/castkms-vm kunit-test
```

`kunit-test` is the fast gate: nine in-kernel suites plus live grant lifetime.
`test` runs that gate by default and then covers capture, cursor, PipeWire,
audio, and the VKMS-derived development facilities. CI sets
`CASTKMS_VM_FAST_GATE=skip` for its independent product job, avoiding a second
matrix and KUnit run. A separate desktop instance checks that Mutter discovers
an attached virtual monitor:

```sh
./scripts/vm/castkms-vm desktop-provision
./scripts/vm/castkms-vm desktop-test
```

GitHub CI runs `make check` on the host, then the fast and product VM lanes,
on every pull request and push to `main`. See
[`docs/vm-testing.md`](docs/vm-testing.md) for commands, coverage, and a
graphical console.

## License

The driver retains the original Linux kernel SPDX declarations and authorship.
See [`COPYING`](COPYING).
