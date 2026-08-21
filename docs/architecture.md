# CASTKMS architecture

## Product boundary

CASTKMS is a virtual presentation sink. The primary path is:

1. A capture agent attaches a monitor and publishes its EDID.
2. A normal DRM compositor discovers, modesets, and renders that connector.
3. The agent captures completed frames into synchronized registered buffers.
4. The agent exports video to PipeWire or another local consumer.

Configfs topology construction, CRC, and writeback remain useful VKMS-derived
development facilities. They must not drive capture UAPI design or weaken the
bounded ownership rules of the primary path. CRC and writeback therefore use
the same current-master content-owner barrier and cannot expose a predecessor's
residual composition.

## Kernel core and UAPI boundary

Capture authorization has a one-way dependency from the grant-fd UAPI to the
kernel-native capture core:

```text
grant fd, DRM files, IDs, ioctls, and events
                    |
                    v
connector-scoped capture authority
                    |
                    v
attachments, streams, buffers, CEC, and composition
```

`struct castkms_capture_authority` is the durable core security object. It owns
connector scope, immutable internal rights, DRM-master/content activation,
revocation, references, and resource-cleanup sequencing. Streams, attachments,
and CEC bindings retain this object and check it in acquisition and frame hot
paths. These checks are core invariants, not UAPI policy.

The grant-fd layer wraps one authority. It owns the device-visible grant ID,
optional close-to-revoke file, holder DRM file, cloned fd, file-local handle
namespaces, and DRM events. It translates public `DRM_CASTKMS_GRANT_*` values
to internal authority rights and states. Core code does not recover authority
from a `drm_file` and does not depend on a grant ID or event structure.

An in-kernel client can create and retain an authority directly, provide
optional stream-cleanup and notification callbacks, and use the same capture
lifecycle without constructing a DRM file or invoking an ioctl. Such a client
does not bypass authorization: it must explicitly create an authority with a
connector, rights, and either master-bound or administrative semantics, and the
same synchronous revoke and content-ownership checks apply. Acquisition is
bracketed by `castkms_capture_authority_begin()`/`end()`, or by the
output-validating `begin_output()` variant before stream construction.

The layering rule is: UAPI adapters may depend on core authority and capture
interfaces; core interfaces must not depend on the grant-fd wrapper.

## Ownership

Authorization is represented by a connector-scoped capture grant fd. The
device's current top-level DRM owner master can create a normal grant bound to
itself. Initial-user-namespace root can explicitly create either a delegated
grant bound to the current owner master or a roaming administrative grant. DRM
lease masters cannot create normal grants because capture-safe content
ownership is device-global. A fresh card fd has no capture authority. The grant
fd has fresh DRM object namespaces, is non-master and unauthenticated, and can
be passed with `SCM_RIGHTS`.

Grant, attachment, and stream are separate lifetimes. The grant is durable
authorization. `ATTACH_MONITOR` makes that grant own connector plug and EDID
state. `START` creates an exclusive, file-local stream for the connector's
current CRTC and mode generation. Stopping a stream leaves the attachment and
grant intact. Detaching stops connector streams but leaves the grant valid for
reattachment. Revocation or final holder close cancels streams, unbinds CEC,
and detaches the monitor.

Normal and delegated grants are active only while their bound `drm_master` is
current and owns the output's capture-safe content. A normal grant is also
revoked when its creator file closes. A delegated grant is holder-lived, so a
one-shot privileged creator can exit; the retained master reference prevents
the grant from following a different compositor. Master loss makes every
existing capture stream obsolete, while either durable grant can revivify if
the same master returns and presents safe content. Explicit root administrative
grants follow safe current-master content across handoffs and keep their
creator file as a revocation anchor. Every holder must create a fresh
mode-specific stream in each master epoch. No grant may capture residual
content from a previous owner. Framebuffers created by the current master and
atomic CRTC compositions carry refcounted master ownership; a non-master
client's framebuffer is ownerless, and no-op commits cannot transfer ownership.

ALSA remains one device-global presentation card. CEC transport ownership is
now a distinct grant right and follows grant suspension/revocation. PipeWire
node visibility remains a userspace policy boundary. See
[`capture-grants.md`](capture-grants.md) for the state machine and teardown
rules.

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
Complete attach, detach, and EDID transitions use the device attachment-
transition mutex; the nested attachment mutex protects the owner pointer and
attached bit. Cross-authority stream cancellation occurs after the transition
mutex and the detaching authority's resource mutex are released. Per-output
slow stream ownership uses `capture.lock`.

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

`EXCLUDE_CURSOR` removes cursor planes only from capture pixels. With the
grant's `READ_CURSOR` right, the event still reports position and a
stream-global image serial. `IMAGE_CHANGED` means the client must read the
bitmap from that event's buffer and cache it for later events; hidden cursors
expose no bitmap. Without `READ_CURSOR`, starting capture requires exclusion
and all cursor metadata is suppressed. CRC and DRM writeback continue to
include the cursor. When those clients overlap an excluded capture,
composition currently pays for separate full and cursor-free results rather
than changing either interface's pixels.

## Audio and CEC

The ALSA device models an HDMI presentation sink: jack, ELD, PCM lifecycle, and
timing are meaningful, but samples are not retained as a capture transport.
PipeWire sink-monitor capture is the supported local bridge today.

CEC `0.1` is opt-in and development-only. Its transport is owned by a grant
with `MANAGE_CEC`, permits one outstanding transmit, and is canceled on
terminal revoke. A normal or delegated grant's transport is suspended on
master loss; an administrative transport survives handoff. Monitor detach
invalidates the physical address and cancels any pending transmit while
retaining the binding for reattachment. CEC still has no advertised state
events or default end-to-end VM coverage.

## Experimental compatibility

Capture major version `0` permits incompatible iteration. Clients must query
the version, formats, limits, synchronization flags, grant-fd capability, and
DMA-BUF import bit; they must not probe optional operations by errno. A stable
major version requires userspace broker/compositor integration and a settled
buffer interoperability contract.
