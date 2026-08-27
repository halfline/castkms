Read this if you are writing a CastKMS capture consumer. `pw-castkms` is a
small end-to-end example of the capture path. It opens a CastKMS primary node,
copies frames into linear `XRGB8888` buffers, and publishes them as one
PipeWire source. It is meant to be read as a protocol and buffer-lifetime
walkthrough, not used as a session broker.

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
   complete sequence: open the device, configure the output, start a capture
   stream, and publish it.
2. [`castkms.c`](castkms.c) validates the device and `0.9` capabilities,
   manages connector attachment, starts and stops capture, and validates DRM
   events.
3. [`castkms-buffer.c`](castkms-buffer.c) creates the destination pool in the
   DRM file's private GEM namespace (kernel buffer objects local to that file)
   and implements implicit or syncobj timeline synchronization.
4. [`pipewire.c`](pipewire.c) is only the transport adapter. It negotiates
   DMA-BUFs and metadata, then moves buffers between PipeWire and CastKMS.
5. [`pw-castkms.h`](pw-castkms.h) contains the shared state. Its grouping is
   intended to make ownership and cleanup order visible.


## Build and exercise

From `tools/pw-castkms`:

```sh
make
```

The repository VM smoke test drives the reference publisher and checks real
PipeWire frame delivery. `run-test.sh` is the smaller equivalent for an
already running CastKMS device and PipeWire environment.
