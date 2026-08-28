# `pw-castkms` example consumer

Read this if you are writing a CastKMS capture consumer. `pw-castkms` is a
small end-to-end example of the capture path. It takes a grant fd (permission
to capture one connector), copies frames into linear `XRGB8888` buffers, and
publishes them as one PipeWire source. It is meant to be read as a protocol
and buffer-lifetime walkthrough, not used as a session broker.

When no EDID file is supplied, the generated virtual monitor advertises basic
stereo HDMI audio. An audio-enabled CastKMS build exposes the corresponding
ALSA sink; a local agent can capture that sink's PipeWire monitor alongside
this video source.

Published nodes use the `Screen` media role and carry the CastKMS card,
connector, CRTC, and stable output index as `api.castkms.*` properties.
`api.castkms.capture=true` distinguishes these product capture sources from
other DRM-backed video nodes.

## Reading order

1. [`pw-castkms.c`](pw-castkms.c) is the application. Its `main()` shows the
   complete sequence: adopt a holder grant, configure the output, start a
   capture stream, and publish it.
2. [`castkms.c`](castkms.c) validates the `0.10` grant and capabilities,
   manages connector attachment, starts and stops capture, and validates DRM
   events.
3. [`castkms-buffer.c`](castkms-buffer.c) creates the destination pool in the
   holder fd's private GEM namespace (kernel buffer objects local to that fd)
   and implements implicit or syncobj timeline synchronization.
4. [`pipewire.c`](pipewire.c) is only the transport adapter. It negotiates
   DMA-BUFs and metadata, then moves buffers between PipeWire and CastKMS.
5. [`pw-castkms.h`](pw-castkms.h) contains the shared state. Its grouping is
   intended to make ownership and cleanup order visible.

The buffer cycle is:

```text
IN_PIPEWIRE -> AVAILABLE -> QUEUED -> READY -> IN_PIPEWIRE
                  |            |         |
             app owns it   CastKMS    frame event
```

For explicit synchronization, the ready timeline is PipeWire's acquire
timeline and the reuse timeline is PipeWire's release timeline. For implicit
synchronization, the same ownership cycle applies without timeline points.
When PipeWire removes the pool after its last consumer pauses, the bridge
stops and drains that CastKMS stream before releasing any destination. A new
PipeWire pool gets a new CastKMS stream and fresh registrations and syncobjs.

## Rules a new consumer should copy

- Receive a grant holder fd from the compositor or broker. Opening the
  primary node does not authorize capture, and the consumer never needs DRM
  master.
- Require capture UAPI `0.10`, `DRM_CASTKMS_CAPTURE_CAP_GRANT_FD`,
  `DRM_CASTKMS_CAPTURE_CAP_GRANT_CONTROL_FD`, an advertised format/modifier
  pair, and all rights needed by the operations the consumer performs.
- Discover only the connector named by `GET_GRANT`. Treat its CRTC route as
  compositor-owned state.
- Create GEM objects, framebuffer IDs, syncobjs, registrations, and queues on
  the holder fd. Handles from an independently opened DRM fd are in the wrong
  namespace.
- Match every completion using stream ID, buffer ID, opaque `user_data`, and
  mode generation before publishing its pixels.
- Stop and rebuild the stream after a mode-generation change. Treat revoke as
  terminal. This example also exits when the grant suspends so a supervisor
  can recreate the publication cleanly.
- Couple registrations and syncobj timelines to a particular destination-pool
  lifetime. Do not unregister a buffer that is still queued; stop the stream
  before destroying or replacing such a pool.
- Keep capture authorization and publication authorization separate. The
  CastKMS grant controls access to pixels; a restricted PipeWire connection
  controls who may consume the published node.

The administrative `castkms-grant-launch` helper is suitable for VM and lab
testing. A production compositor should issue a normal owner-bound grant
directly and pass the resulting fd to its capture agent.

## Build and exercise

From `tools/pw-castkms`:

```sh
make
```

The repository VM smoke test drives the reference publisher and checks real
PipeWire frame delivery. `run-test.sh` is the smaller equivalent for an
already running CastKMS device and PipeWire environment.
