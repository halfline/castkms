# CastKMS architecture

This is for people changing the driver. The userspace grant contract is in
[`capture-grants.md`](capture-grants.md).

CastKMS is a virtual monitor. The product path is:

1. A capture agent attaches a monitor and publishes its EDID, the
   identification blob a real monitor uses to advertise name, modes, and audio
   capabilities.
2. An ordinary DRM compositor discovers that connector, picks a mode, and
   renders it.
3. The agent captures completed frames into synchronized registered buffers.
4. The agent exports video to PipeWire or another local consumer.

Configfs topology construction, per-frame checksums, writeback, and fbdev are
optional VKMS-derived test facilities. They are disabled on the default device.
The capture protocol does not depend on them. Frame checksums and writeback
enforce the same rule as capture: only the current master's own content may be
read, so neither can expose a previous master's leftover composition.

## Capture and writeback

Both copy the composed frame. They are not the same path.

**Capture** is the product. A grant holder starts a stream, queues buffers, and
receives completed frames. Remote-desktop and PipeWire agents use this.

**Writeback** is a VKMS leftover. The DRM master attaches a framebuffer to the
standard writeback connector in an atomic commit, and the driver copies the
frame into that buffer. It is off by default; enable it with
`enable_writeback=1`. Tests and screenshots use it. Capture agents do not.

The two also differ in how they run:

- Capture waits for source writers, then composes later. A slow writer does not
  stall checksumming or writeback.
- Writeback composes inline on the frame-dispatch queue.
- Capture can omit the cursor from the pixels. Writeback always keeps it.

## Layers

CastKMS is built in layers so that the code enforcing authorization never
depends on the ioctl surface that exposes it. The grant-fd UAPI depends
one-way on a kernel-native capture core:

```text
grant fd, DRM files, IDs, ioctls, and events
                    |
                    v
connector-scoped capture authority
                    |
                    v
attachments, streams, buffers, CEC, and composition
```

The durable security object is `struct castkms_capture_authority`. It owns one
connector's scope, its immutable rights, revocation, references, and the order
in which resources are torn down. It decides nothing on its own: it checks
those facts against snapshots from a separate tracker, `castkms_capture_owner`,
which records the current DRM master, the master epoch, and who owns the
composed content. Streams, attachments, and CEC bindings hold the authority
and re-check it both when they start and on the per-frame hot path.

The grant fd is an adapter around one authority. It owns the device-visible
grant ID, optional close-to-revoke file, holder DRM file, file-local handle
namespaces, and DRM events. The holder is an unregistered DRM client file, so
it allocates a fresh primary-minor namespace without ever becoming DRM master
or changing the current master. That construction and its matching release
path live in `castkms_grant_file.c`. The adapter translates public
`DRM_CASTKMS_GRANT_*` values to internal rights and states. Core code does not
recover an authority from a `drm_file`, and it does not depend on a grant ID
or event structure.

The layering rule is: UAPI adapters may depend on core authority and capture
interfaces; core interfaces must not depend on the grant-fd wrapper.

Code linked into the driver can create an authority directly, register
resources with type-neutral cleanup hooks, and use the same capture lifecycle
without a DRM file or an ioctl. That client does not bypass authorization: it
must create an authority with a connector, rights, and either master-bound or
administrative semantics. Acquisition is bracketed by
`castkms_capture_authority_begin()`/`end()`, or by the output-validating
`begin_output()` variant before stream construction. These symbols are an
internal seam, exported only under KUnit so a direct-client test module can
exercise them. They are not a stable cross-module kernel ABI.

## A captured frame

This section follows one frame from commit to captured pixels.

An atomic commit publishes capture-safe content only after its plane and
modeset programming is done. `castkms_frame_dispatch` then fans that frame out
to the three consumers that want pixels: checksumming, writeback, and capture.
It hands the pixel renderer an immutable `struct castkms_frame_stage`, the
ordered render planes, mapped image descriptions, per-plane color operations
held by value, output dimensions, gamma, background, damage, and cursor
metadata. The stage deliberately holds no `drm_crtc_state` or
`drm_plane_state`, so the renderer cannot reach back into live KMS state.
The composer is that renderer, and it consumes both live and snapshot stages
through one `castkms_compose_frame()` entry point.

Capture is the one consumer that cannot render inline, because it may have to
wait for whatever is still drawing into the source. So capture-only work turns
the borrowed live stage into an owned `struct castkms_frame_snapshot`,
deep-copying plane descriptions, color state, gamma, framebuffer references,
and mappings. Each source writer's fence is captured under its reservation
lock, and a snapshot read fence is published before that lock is dropped. The
deferred work then lives in `castkms_capture_job.c`: a workqueue waits for
those writers, composes, releases the snapshot, and signals its read fence.
Because it is deferred, a slow source writer does not stall unrelated checksum
or writeback jobs on the frame-dispatch queue.

The capture core's unit of work is `struct castkms_capture_request`. It
carries synchronization points and a completion callback. Completion supplies
a transport-neutral result: status, timing, damage, and cursor metadata. The
DRM UAPI layer owns file-local stream and buffer IDs, reserves the
`drm_pending_event`, embeds the core request in that event adapter, and
translates the callback result into the public event. The capture core neither
reserves DRM file events nor handles ioctl request structures.

Capture implementation state is private to `castkms_capture_internal.h`.
`castkms_capture.c` owns streams, output mode generations, cancellation, and
vblank selection. `castkms_capture_buffer.c` owns buffer registration,
synchronization, the state machine, and completion delivery;
`castkms_capture_cursor.c` owns cursor metadata and bitmap extraction; and
`castkms_capture_job.c` owns deferred snapshot rendering. Monitor attachment,
detachment, and EDID ioctls live in `castkms_connector_uapi.c`, leaving
`castkms_capture_uapi.c` responsible only for stream, buffer, cursor-read, and
DRM-event translation.

For a successful request, a buffer normally follows this route:

```text
IDLE -> PREPARING -> WAITING_REUSE -> QUEUED -> IN_FLIGHT -> COMPLETING -> IDLE
```

The driver puts the buffer in `PREPARING` while it gathers and installs the
fences or timeline points needed for the request.

`WAITING_REUSE` means that a previous reader or writer still owns the buffer;
it is skipped when there is no dependency to wait on. In `COMPLETING`, the
driver has removed the request from the buffer and saved its result for the
callback. The buffer returns to `IDLE` before that callback runs and before its
producer fence is signaled.

Other transitions occur when setup fails, a request is canceled, or the CRTC
state already contains capture work:

- If the driver cannot prepare the fences or timeline points, `PREPARING`
  returns directly to `IDLE`.
- Failure or cancellation moves a buffer from `WAITING_REUSE`, `QUEUED`, or
  `IN_FLIGHT` to `COMPLETING`, so the callback still receives a result and the
  producer fence is signaled with the appropriate error.
- If a buffer is chosen at vblank but the pending CRTC state already contains
  capture work, the buffer moves from `IN_FLIGHT` back to `QUEUED`. A later
  vblank can try it again.

An output holds at most one queued and one in-flight buffer at a time.
Selection at vblank never waits: if a buffer's reuse dependency is still
unresolved, the frame is skipped and the dropped-frame count increases up to
the largest value the counter can represent.

The reserved DRM event reports metadata and never transfers ownership. The
implicit reservation fence or explicit ready timeline point is the ownership
boundary, and the buffer is already `IDLE` when the adapter callback publishes
the event and the producer fence subsequently signals. A client may
unregister or requeue from a fence waiter without first reading the event.
Cancellation preserves the same rule and signals an error on the producer
fence.

`START` snapshots the active mode and returns its generation. Registered
destinations are sized for that generation. A modeset advances the output
generation and completes queued or in-flight work with `-ESTALE` and
`MODE_CHANGED`. The client must stop the old stream, start a new one, and
register new buffers. Version 0.x may replace this restart model, but clients
must not infer adoption from the generation reported by a cancellation event.

## Ownership

The grant UAPI is described in [`capture-grants.md`](capture-grants.md). The
architectural invariant underneath it is: no client may capture residual
content from a previous owner.

Framebuffers created by the current DRM master, and atomic CRTC compositions
of those framebuffers, carry refcounted master ownership. A non-master
client's framebuffer is ownerless, and a no-op commit cannot transfer
ownership. Grant, attachment, and stream are separate lifetimes: the grant is
durable authorization, attachment is durable connector state, and a capture
stream is bound to one CRTC and mode generation. Master loss makes every
existing capture stream obsolete; the holder must create a fresh stream in
each master epoch.

See [`capture-grants.md`](capture-grants.md) for creation forms, states,
errors, and teardown.

## Cursor and damage

The cursor can be captured with the frame, delivered separately, or left out
entirely, depending on the grant's rights.

`EXCLUDE_CURSOR` removes cursor planes from the capture pixels only. With the
grant's `READ_CURSOR` right, the event still reports the cursor's position and
a stream-global image serial; `IMAGE_CHANGED` tells the client to read the new
bitmap from that event's buffer and cache it for later events, and a hidden
cursor exposes no bitmap. Without `READ_CURSOR`, capture must be started with
exclusion and all cursor metadata is suppressed. Frame checksums and DRM
writeback always keep the cursor. When one of those overlaps an excluded
capture, composition produces two results, a full one and a cursor-free one,
rather than altering either interface's pixels.

Damage is the bounding rectangle of what changed in the current atomic state.
`FULL_DAMAGE` marks mode, plane-set, and color-management changes, and any
full-frame clip. Treat it as a hint about what changed, not as permission to
leave the rest of a buffer undefined: every successful capture and writeback
buffer holds a complete frame, and a frame checksum is always a full-frame
hash.

Registered capture buffers rotate independently and may contain frames from
different points in history. Clipping composition to one commit's damage would
leave stale pixels unless the driver first maintains a complete output cache or
accumulates damage separately for every destination.

## Audio and CEC

Audio and CEC make the virtual output behave like a real HDMI monitor; neither
is a way to capture data out of the kernel.

The ALSA device is a single device-global HDMI presentation card. Jack
detection, ELD (the short audio-capability list derived from EDID), the PCM
lifecycle, and timing are all real, but the samples themselves are not kept for
any capture path. To capture audio locally, read the PipeWire sink's monitor
instead; who may see that PipeWire node is a userspace policy decision.

HDMI-CEC (Consumer Electronics Control) is the command channel HDMI devices
use for power, volume, and input switching. CEC `0.1` is opt-in and
development-only. Its transport is owned by a grant with `MANAGE_CEC`, permits
one outstanding transmit, and is canceled on terminal revoke. A normal or
delegated grant's transport is suspended on master loss; an administrative
transport survives handoff. Monitor detach invalidates the physical address
and cancels any pending transmit while retaining the binding for
reattachment.

CEC follows the same core-to-UAPI direction as capture. `castkms_cec_core`
owns connector CEC state, transaction cookies and timeouts, and a
transport-neutral binding whose adapter supplies a prepared request with a
publish/cancel callback. `castkms_cec_uapi` performs DRM object lookup and
grant validation, retains the active DRM file while preparing an event,
translates core state to public UAPI structures, and owns every
`drm_pending_event`. The core contains no grant-fd, DRM-file, event, or
public-UAPI dependency:

```text
castkms_cec_uapi -> grant adapter -> capture authority
        |                                ^
        v                                |
castkms_cec_core -> authority resource hooks
```

## Locking and completion

This is the reference for the lock order. The rule to remember is that heavy
work (fence waits, event delivery, subsystem cleanup) always happens outside
the hot spinlocks, so completion callbacks never run inside a critical section.
The specifics follow.

Slow file operations hold the file's capture mutex while looking up streams.
Complete attach, detach, and EDID transitions use the device
attachment-transition mutex; the nested attachment mutex protects the owner
pointer and attached bit. Cross-authority stream cancellation occurs after the
transition mutex and the detaching authority's resource mutex are released.
Per-output slow stream ownership uses `capture.lock`.

Authority acquisition precedes the authority resource mutex. Resource hooks
run under that mutex and may then take their subsystem lock; CEC uses this to
serialize suspension and revocation with transport bind/unbind before taking
its per-output spinlock. A terminal revoke publishes its status under the
authority lock, releases it, and then performs synchronous resource cleanup.

The spinlock order for atomic, vblank, and completion paths is:

```text
castkms_output.lock
  -> castkms_capture_output.state_lock
    -> castkms_output.dispatch_lock
```

Not every path needs all three, but reverse acquisition is forbidden. Buffer
completion is detached under `state_lock`, then all caller-owned spinlocks are
released. Delivery transitions the buffer to `IDLE`, invokes the core request
callback, and then signals the producer fence. The DRM adapter callback sends
or cancels its reserved event; neither fence callbacks nor event delivery run
inside the capture state critical section. Stream teardown waits for accepted
request callbacks so common DRM-file postclose cannot invalidate their event
state.

## Source layout

Headers are narrow: each owns one subsystem. An outer `castkms_uapi_device`
shell combines the core device with an opaque pointer to the grant adapter's
private registry; the core device contains no grant state. The grant ID
xarray, lock, and allocation cursor live only in `castkms_grant.c`. CEC core
initialization takes a `drm_device` directly and does not include the CastKMS
device layout.

Each area of behavior has exactly one owning file:

| Area | Owner |
|---|---|
| Driver-private `drm_file` open, postclose, and release | `castkms_file.c` |
| Never-master grant DRM file | `castkms_grant_file.c` |
| Grant IDs, adapter, and UAPI translation | `castkms_grant.c` |
| Capture authority and resource hooks | `castkms_capture_authority.c` |
| Current master and composed-content facts | `castkms_capture_owner.c` |
| Streams, output scheduling, and mode generation | `castkms_capture.c` |
| Buffer state, synchronization, and completion | `castkms_capture_buffer.c` |
| Captured cursor state and bitmap extraction | `castkms_capture_cursor.c` |
| Capture stream/buffer ioctl and DRM-event adapter | `castkms_capture_uapi.c` |
| Monitor attachment and EDID ioctl adapter | `castkms_connector_uapi.c` |
| Deferred capture execution | `castkms_capture_job.c` |
| Immutable source snapshots and fences | `castkms_snapshot.c` |
| CRTC scheduling and checksum/writeback/capture mux | `castkms_frame_dispatch.c` |
| Pixel rendering | `castkms_composer.c` |
| Driver CRTC atomic state | `castkms_crtc.h` |
| Per-output runtime assembly | `castkms_output.h`, via `castkms_capture_output.h` and `castkms_frame_dispatch_demand.h` |
| Topology construction | `castkms_topology.c` |
| Frame-checksum declarations | `castkms_crc.h` |
| Private ioctl dispatch and grant-file policy | `castkms_ioctl_table.inc` |
| DRM-core grant-file allowlist | `castkms_grant_core_ioctl_table.inc` |
| CEC transport | `castkms_cec_core.c` |
| CEC ioctl and DRM events | `castkms_cec_uapi.c` |

`castkms_file.c` is the single owner of driver open, postclose, and special
release dispatch. Capture and grant adapters contribute only their subsystem
cleanup. DRM core ioctl access from a grant file is a separate default-deny
metadata table, because those commands are owned by DRM rather than the
CastKMS UAPI registry.

`make check-ioctls` compares `castkms_ioctl_table.inc` with every public
`DRM_IOCTL_CASTKMS_*` definition, so a new private command cannot omit its
grant-holder authorization decision. It also checks the DRM-core grant
allowlist for duplicate or malformed metadata, while KUnit exercises every
allowed entry and representative denied master, modeset, atomic, and DMA-BUF
import commands. Kernel dimensions live independently in `castkms_limits.h`;
the UAPI adapters use compile-time assertions to catch protocol drift.
`make check-architecture` enforces the header and dependency rules above. The
audio/CEC build matrix also checks CEC's undefined-symbol set so a grant,
DRM-file, or event dependency cannot appear in the core object.

`scripts/architecture-layers.txt` assigns every production C source and header
to exactly one layer. The architecture check rejects stale, duplicate, or
unclassified paths and validates forbidden include directions from that
complete manifest, so adding a file cannot silently bypass the core/UAPI
boundary checks.

Ownership tracking and authority evaluation are distinct. The owner tracker
publishes coalesced generation changes through immutable notification
operations supplied at device initialization; authority registers the
reconciliation callback. The tracker neither imports nor calls authority
policy, so DRM callback and atomic-publication code does not contain UAPI
grant rules.

Authority resources provide type-neutral selection, suspend, and revoke
hooks. CEC bindings and capture stream adapters both register resources.
Terminal revocation removes every resource synchronously; a master-epoch
transition or connector disconnect selects only resources invalidated by that
reason. The grant adapter has no capture-specific teardown callback and does
not include `castkms_capture_uapi.h`. Connector attachment cleanup is
initiated by the connector layer.

## Experimental compatibility

Capture major version `0` permits incompatible iteration. Clients must query
the version, formats, limits, synchronization flags, grant-fd capability, and
DMA-BUF import bit; they must not probe optional operations by errno. A stable
major version needs userspace broker and compositor integration and a settled
buffer interoperability contract.
