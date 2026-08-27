# CastKMS capture grants

This is the contract for compositor and capture-agent authors. Driver internals
that implement it are in [`architecture.md`](architecture.md).

A **grant** is a capability file descriptor for one CastKMS connector. It
authorizes the holder to attach a monitor, publish EDID, capture pixels, read
cursor data, and speak HDMI-CEC, whichever rights were requested at creation.
Opening `/dev/dri/cardN` does not confer any of those rights. Sensitive ioctls
require the grant fd, which is a newly created DRM file. The fd can be
duplicated or passed over a Unix socket (`SCM_RIGHTS`); all copies represent
the same capability, and closing the last copy ends it.

The grant is how a capture agent reads pixels. Do not use the DRM writeback
connector for that. Writeback is a master-side test copy, off by default, and
is not part of this contract.

The driver does not identify processes, application IDs, cgroups, executable
names, or sandbox runtimes. Possession of the grant fd is the authorization.
That is what keeps a Flatpak with DRI device access from capturing the
virtual monitor.

The boundary does not protect against root, kernel compromise, a compromised
grant creator or revoker, or a creator that deliberately passes its grant to
an untrusted recipient. PipeWire publication is a separate boundary: a capture
producer must keep its audio and video nodes private or assign per-client
permissions.

## Which grant to create

**DRM master** is the process currently allowed to modeset the card, usually
the compositor.

- Writing a compositor that owns the output? Create a **normal** grant. No
  root helper is required. Closing the compositor's DRM file revokes it.
- Writing a one-shot root helper that should outlive the helper but die with
  that compositor? Create a **delegated** grant. The helper can exit; the
  grant stays bound to the current compositor's master identity.
- Writing a lab or diagnostic tool that should follow whichever compositor is
  current? Create an **administrative** grant. Keep the creating file open as
  the revoker.

`DRM_IOCTL_CASTKMS_CREATE_GRANT` encodes those forms as follows. The two
creation flags are mutually exclusive. A privileged caller that is not the
current master, and passes no flag, is denied, rather than silently being
handed an administrative grant that follows whichever compositor is current.
Host root here means `CAP_SYS_ADMIN` in the initial user namespace (true host
root, not a container).

| Creation flags | Required caller | Master binding | Creator-file close |
|---|---|---|---|
| none | Current top-level DRM owner master | Caller's `drm_master` | Revokes |
| `CREATE_DELEGATED` | Host root, not current master | Current top-level owner master | No effect |
| `CREATE_ADMIN` | Host root | None; follows current safe owner | Revokes |

Delegated creation returns `-EAGAIN` if there is no current owner master or
the caller is itself current. Opening a masterless card implicitly makes the
opener master, so a helper must not bind a grant to that accidental identity.

A DRM **lease** master is a client given only a subset of the card's
resources. It cannot create a normal grant, even for a connector included in
its lease. Capture-safe content ownership is device-global, so a nested lease
grant would promise authority the driver cannot represent.

A delegated grant retains a reference to that exact `drm_master`. It cannot
activate for a later login session or a restarted compositor with a new
master identity. The current compositor may query and revoke delegated grants
bound to itself, so it does not depend on the helper remaining alive. Host
root may query or revoke any grant by its live ID.

An administrative grant follows safe content across compositor handoffs. Root
may create it while the device is masterless; the grant is then dormant until
a current master establishes capture-safe content. Because opening a
masterless card makes the opener master, an administrative helper must drop
that accidental master immediately while retaining the same file as revoker.

Grant-creation policy flags and returned-file flags use separate fields.
`flags` accepts either `DRM_CASTKMS_GRANT_CREATE_DELEGATED` or
`DRM_CASTKMS_GRANT_CREATE_ADMIN`; `fd_flags` accepts only `O_NONBLOCK`.
Close-on-exec is unconditional and is not requested through either field.

The creator file for a normal or administrative grant retains revocation
authority even while its DRM master is inactive. A delegated creator retains
no association after the ioctl returns.

## The grant file

The returned file:

- has fresh GEM-handle, framebuffer, syncobj, event, and driver-private
  namespaces;
- is not current DRM master and is marked unauthenticated;
- cannot become DRM master through `SET_MASTER` or `DROP_MASTER`;
- cannot create child grants;
- is always close-on-exec and may optionally be nonblocking;
- supports ordinary non-master DRM buffer and synchronization ioctls.

Create GEM objects, framebuffer IDs, syncobjs, registrations, and queues on
this file. Handles from an independently opened DRM fd are in the wrong
namespace.

## Rights

Rights are immutable and connector-scoped:

| Right | Meaning |
|---|---|
| `CAPTURE_PIXELS` | Start streams and register or queue capture buffers. |
| `MANAGE_ATTACHMENT` | Attach a remote monitor. |
| `UPDATE_EDID` | Provide an EDID while attaching a remote monitor. |
| `READ_CURSOR` | Include cursor pixels or receive cursor metadata. |
| `MANAGE_CEC` | Bind and operate HDMI-CEC (the HDMI command channel) on the connector. |

`ATTACH_MONITOR` with an EDID requires both attachment and EDID rights. Unknown
rights are rejected.

Only one live grant with `MANAGE_ATTACHMENT` may exist per connector. Capture
is exclusive per output. Additional capture-only grants are structurally
possible, but attachment ownership is singular.

## Validity and pixel activation

Permanent validity and temporary pixel activation are separate.

A grant remains valid until one of these terminal events:

- explicit revoker-file or administrative revocation;
- creator/revoker DRM file close for a normal or administrative grant;
- final holder-file close;
- connector/device teardown or unplug.

A delegated grant omits the second event. Its terminal lifetime is holder
close, explicit revocation by its bound current owner or root, or device
teardown.

`GET_GRANT.state` reports whether pixel capture may proceed. Several states
turn on *capture-safe content*, pixels the current master owns and may safely
read, which the **Capture-safe content** section below defines in full:

| State | Meaning |
|---|---|
| `PENDING` | No attached, routed, active output is ready yet. |
| `ACTIVE` | The current master owns a capture-safe composition. |
| `SUSPENDED_NO_MASTER` | No DRM master is current. |
| `SUSPENDED_OTHER_MASTER` | A normal or delegated grant's bound master is not current. |
| `SUSPENDED_FOREIGN_CONTENT` | The authority is current, but residual or mixed-owner pixels are not safe. |
| `REVOKED` | The grant is permanently inert. |

For a normal or delegated grant, pixel capture is usable only when its bound
`drm_master` is current and owns safe content on the authorized connector. A
drop suspends it synchronously. If the same master returns, or returns after an
intervening master, the durable grant can become active again after it owns a
safe composition. A compositor restart creates a new master identity and
cannot revive the old grant. Every capture stream is canceled on master loss
and must be created again, including streams held by administrative grants.

An administrative grant does not enter `SUSPENDED_OTHER_MASTER`; it follows
the current master's safe content across handoffs. It still cannot capture
while there is no current master or while the output contains residual foreign
content. Non-pixel administrative rights, such as attachment or EDID
management, remain authorized during a handoff or masterless interval.

Grant `fdinfo` reports the most recently reconciled state. Use `GET_GRANT`
when an authoritative point-in-time state is required.

The direct operation errors are:

- `-EACCES`: no grant, wrong connector, or missing right;
- `-EKEYREVOKED`: terminal revocation;
- `-EAGAIN`: temporary master suspension or an obsolete capture stream;
- `-ESTALE`: unsafe content ownership or a stale mode generation;
- `-ENOLINK`: the connector is attached but has no active routed output;
- `-ENODEV`: device teardown or unplug.

## Capture-safe content

Current DRM master alone is not enough. A new master must not capture the
previous master's residual frame, and a returning master must not capture the
last frame presented by an intervening owner.

CastKMS stamps a framebuffer with a refcounted DRM-master pointer only when
the file creating it is current master. A non-master primary client may be
associated with the current `drm_master`, but that association is not content
ownership and produces an ownerless framebuffer. An **atomic commit**, the
display update in which a compositor submits a whole new screen configuration
at once, derives its capture owner from every visible plane:

- all visible framebuffers must have the same owner;
- ownerless or mixed-owner visible planes make the composition unsafe;
- disabling an output clears its capture owner;
- an active blank output transfers ownership only through a mode, active, or
  background-changing commit;
- a no-op commit cannot claim old pixels.

The commit publishes the new owner only after plane and modeset programming.
Capture queueing, vblank selection (choosing a frame at the **vblank**, the
gap between frames when a new one can be shown), stream startup, and a frame
checksum all require the published owner to equal the current master. While an
atomic commit is in flight the output is marked unsafe.

CastKMS withholds a frame rather than guessing that retained pixels belong to
the new owner. Frame checksums and writeback are additional pixel-derived
export paths, not the capture protocol. A newly installed master cannot
checksum a leftover frame or use a no-op writeback commit to observe the
previous master's residual composition. It may use them after its atomic state
establishes content owned by that master.

## Grant, attachment, and stream lifetime

The grant is durable authorization. Attachment is durable connector state
owned by the grant. A capture stream is mode-specific:

```text
grant       survives normal display reconfiguration
attachment  survives normal display reconfiguration
stream      bound to one CRTC and mode generation
buffers     bound to one stream generation and dimensions
```

Resolution, refresh, timing, blank/unblank, and CRTC reassignment never
revoke the grant. A modeset increments `mode_generation`, returns queued work
with `-ESTALE` and `MODE_CHANGED`, and requires a replacement stream and
mode-sized buffers on the same grant fd.

## Revocation and events

Revocation first makes the grant permanently unable to authorize another
attachment, EDID, capture, cursor, or CEC operation. The authority keeps one
list of the capture streams and CEC bindings that must be cleaned. It walks
that list in registration order, so clients must not rely on capture cleanup
happening before or after CEC cleanup. The complete sequence is:

1. Walk that list. Cleaning a stream cancels queued and in-flight work and
   places an error on each producer fence. Cleaning a CEC binding aborts and
   unbinds its transport work.
2. Send the pre-reserved `GRANT_REVOKED` event.

The first step finishes every registered entry before event delivery begins.

The driver reserves the reliable revoke event when the grant is created, so it
does not need to allocate memory while cleaning up. `GRANT_STATE` events report
changes that do not revoke the grant. They are advisory and may be combined or
may not be sent if event reservation fails; `GET_GRANT` is authoritative. A
revoked fd remains usable for event reads, state queries, ordinary DRM resource
release, and close, but can never regain authority.

Temporary foreign-content states leave the stream allocated; queue and vblank
eligibility checks return or suppress `-ESTALE` work until the composition
becomes safe again.

## Kernel implementation

The fd is an adapter around a kernel-native capture authority. The authority
contains connector scope, rights, and revocation state, and evaluates them
against snapshots supplied by the DRM/content ownership tracker. The adapter
contains only UAPI concerns: grant ID, optional close-to-revoke file, holder
DRM file, fd lifetime, and DRM events. Capture streams, connector attachments,
and CEC transports retain the authority rather than the adapter.

Trusted code linked into the driver may create an authority directly and use
the same core lifecycle without fabricating a `drm_file`. The constructor is
explicit and applies the same rights, master/content, suspension, and
revocation rules; there is no nullable-authority or implicit privileged bypass
in the capture core. The client brackets connector operations with
`castkms_capture_authority_begin()`/`end()` and uses
`castkms_capture_authority_begin_output()` before constructing a stream.
These functions are not a stable exported kernel ABI. KUnit-only exports let a
separate direct client exercise this seam without crossing the UAPI adapter.

The never-master DRM file is constructed in `castkms_grant_file.c`: an
unregistered DRM client supplies the fresh primary file and the matching
special release path. Common driver-private DRM-file state and open/postclose
handling live in `castkms_file.c`.

Master callbacks perform only synchronous authority-state changes and queue a
worker. Stream, fence, connector, audio, and CEC cleanup occurs outside the
DRM master mutex. Deferred cleanup removes only streams whose authority
generation predates the observed cleanup sequence, so a replacement stream
created after reacquisition cannot be collected by an older work item.

Authority checks are repeated in the vblank path so revocation racing stream
startup cannot disclose a later frame. Complete attachment transitions are
serialized through their hotplug, audio, and CEC side effects.
CEC teardown blocks replacement binding until any transport callback that is
preparing a request has retired. The DRM adapter owns event reservation and
publication; CEC core owns only the publish-or-cancel decision.

`GET_GRANT` reads connector routing under the interruptible connection mutex
rather than locking the complete modeset object set. Grant `fdinfo` does not
acquire KMS modeset locks. Frame checksumming cannot be turned on against
unsafe content, vblank does not arm checksum work while content is unsafe, and
a worker for an old CRTC state rechecks that state's owner before publishing a
checksum.
CEC-core callbacks do not wait while holding the core adapter mutex.
