# CASTKMS architecture

## Product boundary

CASTKMS is a virtual presentation sink. The primary path is:

1. A capture agent attaches a monitor and publishes its EDID.
2. A normal DRM compositor discovers, modesets, and renders that connector.
3. The agent captures completed frames into synchronized registered buffers.
4. The agent exports video to PipeWire or another local consumer.

Configfs topology construction, CRC, and writeback remain useful VKMS-derived
development facilities. They must not drive capture UAPI design or weaken the
bounded ownership rules of the primary path.

## Ownership

Attachment and capture are deliberately separate lifetimes today.
`ATTACH_MONITOR` gives one DRM file ownership of connector plug state and EDID.
`START` gives one file an exclusive, file-local stream for one CRTC. Stopping a
stream releases its buffers but leaves a monitor attached; detaching does not
implicitly define a reusable capture session. Closing the file cancels its
streams, releases their buffers, unbinds its CEC transports, and detaches its
monitors.

ALSA exposes one device-global presentation card, and CEC has a separate
per-connector transport owner. These are not yet grouped with attachment and
capture in a session or lease. All mutating capture and transport ioctls remain
root-only while that authorization and lifetime model is unresolved.

## Capture buffer state

A registered buffer moves through:

```text
IDLE -> WAITING_REUSE -> QUEUED -> IN_FLIGHT -> COMPLETING -> IDLE
```

`WAITING_REUSE` is skipped when no dependency exists. One output may have one
queued and one in-flight buffer. Selection happens at vblank without waiting;
an unresolved reuse dependency makes the frame ineligible and increments the
bounded dropped-frame count.

A reserved DRM event reports metadata. It never transfers ownership. The
implicit reservation fence or explicit ready timeline point is authoritative,
and the buffer is already `IDLE` when that producer fence signals. A client may
therefore unregister or requeue directly from a fence waiter without first
reading the event. Cancellation preserves the same rule and signals an error on
the producer fence.

## Mode generations

`START` snapshots the active mode and returns its generation. Registered
destinations are sized for that generation. A modeset advances the output
generation and completes queued or in-flight work with `-ESTALE` and
`MODE_CHANGED`; the old stream remains generation-bound. The client must stop
it, start a new stream, and register new buffers. Version 0.x may replace this
restart model, but clients must not infer adoption from the generation reported
by a cancellation event.

## Locking and completion delivery

Slow file operations hold the file's capture mutex while looking up streams.
Connector attachment uses the device attachment mutex. Per-output slow stream
ownership uses `capture.lock`.

The spinlock order for atomic, vblank, and completion paths is:

```text
castkms_output.lock
  -> castkms_capture_output.state_lock
    -> castkms_output.composer_lock
```

Not every path needs all three, but reverse acquisition is forbidden. Buffer
completion is detached under `state_lock`, then all caller-owned spinlocks are
released. Delivery transitions the buffer to `IDLE`, publishes the reserved
DRM event while it is protected from file-close cleanup, and then signals the
producer fence. Fence callbacks and event delivery therefore never execute
inside the capture state critical section.

## Source snapshots

Capture-only composition snapshots framebuffer references, mappings, plane
state, color operations, gamma, damage, and cursor metadata on the ordered
composer queue. Existing source writer fences are captured while holding each
reservation lock, and a snapshot read fence is published before unlocking.
The capture workqueue waits for the captured writers, composes, then releases
the snapshot and signals its read fence. A slow source writer therefore does
not block unrelated CRC or writeback jobs on the composer queue.

## Damage semantics

Damage is the bounding rectangle of changes represented by the current atomic
state. `FULL_DAMAGE` covers mode, plane-set, and color-management changes as
well as a full-frame clip. It is encoder metadata, not permission to leave the
rest of a destination undefined: every successful capture and writeback buffer
currently contains a complete frame, and CRC is a full-frame hash.

Registered capture buffers rotate independently and may contain frames from
different points in history. Clipping composition to one commit's damage would
leave stale pixels unless the driver first maintains a complete output cache or
accumulates damage separately for every destination. Any damage optimization
must add one of those mechanisms and preserve full-frame CRC policy.

## Cursor exclusion

`EXCLUDE_CURSOR` removes cursor planes only from capture pixels. The event still
reports position and a stream-global image serial. `IMAGE_CHANGED` means the
client must read the bitmap from that event's buffer and cache it for later
events; hidden cursors expose no bitmap. CRC and DRM writeback continue to
include the cursor. When those clients overlap an excluded capture, composition
currently pays for separate full and cursor-free results rather than changing
either interface's pixels.

## Audio and CEC

The ALSA device models an HDMI presentation sink: jack, ELD, PCM lifecycle, and
timing are meaningful, but samples are not retained as a capture transport.
PipeWire sink-monitor capture is the supported local bridge today.

CEC `0.1` is opt-in and development-only. It has a distinct owner, one
outstanding transmit, no advertised state events, and no default VM coverage.
It must either gain a complete tested lifecycle or remain outside the session
agent product story.

## Experimental compatibility

Capture major version `0` permits incompatible iteration. Clients must query
the version, formats, limits, synchronization flags, and DMA-BUF import bit;
they must not probe optional operations by errno. A stable major version
requires a settled seat/session authorization model, mode-change lifecycle,
and buffer interoperability contract.
