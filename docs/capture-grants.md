# CastKMS capture grants

## Security boundary

Opening a CastKMS primary node does not authorize capture. Sensitive ioctls
require a connector-scoped grant carried by a newly created DRM file. The
grant fd can be duplicated or passed with `SCM_RIGHTS`; all copies represent
the same capability and final close ends it.

This protects against applications and sandboxes that can open `/dev/dri`,
including a Flatpak with DRI device access. The driver does not identify
processes, application IDs, cgroups, executable names, or sandbox runtimes.
Possession of the grant fd is the authorization.

Internally, the fd is an adapter around a kernel-native capture authority. The
authority contains connector scope, rights, master/content activation, and
revocation state. The adapter contains only UAPI concerns: grant ID, optional
close-to-revoke file, holder DRM file, fd lifetime, and DRM events. Capture
streams, connector attachments, and CEC transports retain the authority rather
than the adapter.

This split also provides a non-UAPI kernel path. A trusted in-kernel client may
create an authority directly and use the same core lifecycle without fabricating
a `drm_file`. The constructor is explicit and applies the same rights,
master/content, suspension, and revocation rules; there is no nullable-authority
or implicit privileged bypass in the capture core. The client brackets
connector operations with `castkms_capture_authority_begin()`/`end()` and uses
`castkms_capture_authority_begin_output()` before constructing a stream.

The boundary does not protect against root, kernel compromise, a compromised
grant creator or revoker, or a creator that deliberately passes its grant to
an untrusted recipient. PipeWire publication is a separate boundary: a capture
producer must keep its audio and video nodes private or assign per-client
permissions.

## Authority to create a grant

`DRM_IOCTL_CASTKMS_CREATE_GRANT` has three explicit creation forms:

| Creation flags | Required caller | Master binding | Creator-file close |
|---|---|---|---|
| none | Current top-level DRM owner master | Caller's `drm_master` | Revokes |
| `CREATE_DELEGATED` | Initial-user-namespace `CAP_SYS_ADMIN`, not current master | Current top-level owner master | No effect |
| `CREATE_ADMIN` | Initial-user-namespace `CAP_SYS_ADMIN` | None; follows current safe owner | Revokes |

The two flags are mutually exclusive. A privileged non-master call with no
flag is denied rather than silently receiving roaming administrative
authority. Delegated creation returns `-EAGAIN` if there is no current owner
master or the caller is itself current. This prevents a one-shot helper that
automatically acquired master while opening an otherwise masterless primary
node from binding a grant to its own transient master identity.

A DRM lease master is not the device owner master and cannot create a normal
grant, even for a connector included in its lease. Capture-safe framebuffer and
composition ownership currently use the device-global owner-master identity,
so accepting nested lease delegation would promise authority that the content
tracking model cannot represent.

This distinction is deliberate. A compositor can create a normal grant for
its own output without a root broker. A one-shot privileged helper can use
`CREATE_DELEGATED` to create the same master-bound capability on the current
compositor's behalf, pass the holder fd with `SCM_RIGHTS`, close its DRM file,
and exit. The authority retains a reference to that exact `drm_master`, so it
cannot activate for a later login session or a restarted compositor with a new
master identity.

`CREATE_ADMIN` is the explicit roaming form for system-wide diagnostics or a
long-lived broker. It follows safe content across compositor/master handoffs,
so its creator file remains a close-to-revoke anchor. Root may create it while
the device is masterless; the grant is then dormant until a current master
establishes capture-safe content.

The returned file:

- has fresh GEM-handle, framebuffer, syncobj, event, and driver-private
  namespaces;
- is not current DRM master and is marked unauthenticated;
- cannot create child grants;
- is always close-on-exec and may optionally be nonblocking;
- supports ordinary non-master DRM buffer and synchronization ioctls.

Grant-creation policy flags and returned-file flags use separate fields.
`flags` accepts either `DRM_CASTKMS_GRANT_CREATE_DELEGATED` or
`DRM_CASTKMS_GRANT_CREATE_ADMIN`; `fd_flags` accepts only `O_NONBLOCK`.
Close-on-exec is unconditional and is not requested through either field.

The creator file for a normal or administrative grant retains revocation
authority even while its DRM master is inactive. A delegated creator retains
no association after the ioctl returns. Initial-user-namespace root may revoke
any grant by its live ID, including a delegated grant after creator exit.

## Validity and pixel activation

Permanent validity and temporary pixel activation are separate.

A grant remains valid until one of these terminal events:

- explicit revoker-file or administrative revocation;
- creator/revoker DRM file close for a normal or administrative grant;
- final holder-file close;
- connector/device teardown or unplug.

A delegated grant deliberately omits the second event. Its terminal lifetime
is holder close, explicit root revocation, or device teardown.

`GET_GRANT.state` reports pixel-capture activation:

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

The direct operation errors are:

- `-EACCES`: no grant, wrong connector, or missing right;
- `-EKEYREVOKED`: terminal revocation;
- `-EAGAIN`: temporary master suspension or an obsolete capture stream;
- `-ESTALE`: unsafe content ownership or a stale mode generation;
- `-ENOTCONN`: the connector is detached;
- `-ENODEV`: device teardown or unplug.

## Capture-safe content ownership

Current DRM master alone is insufficient. A new master must not capture the
previous master's residual frame, and a returning master must not capture the
last frame presented by an intervening owner.

CastKMS therefore stamps a framebuffer with a refcounted DRM-master pointer
only when the file creating it is current master. A non-master primary client
may be associated with the current `drm_master`, but that association is not
content ownership and produces an ownerless framebuffer. Atomic CRTC state
derives its capture owner from every visible plane:

- all visible framebuffers must have the same owner;
- ownerless or mixed-owner visible planes make the composition unsafe;
- disabling an output clears its capture owner;
- an active blank output transfers ownership only through a mode, active, or
  background-changing commit;
- a no-op commit cannot claim old pixels.

Commit tail publishes the new owner only after plane and modeset programming.
Capture queueing, vblank selection, stream startup, and CRC sampling require
the published owner to equal the current master. During an atomic update the
output is explicitly marked unsafe. A CRC source cannot be enabled against
unsafe content, vblank does not arm CRC work while content is unsafe, and a
worker for an old CRTC state rechecks that state's owner before publishing a
sample.

This is intentionally conservative. CastKMS withholds a frame rather than
guessing that retained pixels belong to the new owner.

The same owner check gates the legacy DRM writeback connector. CRC and
writeback are additional pixel-derived export paths, so a newly installed
master cannot use a CRC sample or no-op writeback commit to observe the
previous master's residual composition. It may use them after its atomic state
establishes content owned by that master.

## Rights

Rights are immutable and connector-scoped:

| Right | Operations |
|---|---|
| `CAPTURE_PIXELS` | Start streams and register or queue capture buffers. |
| `MANAGE_ATTACHMENT` | Attach or detach a remote monitor. |
| `UPDATE_EDID` | Publish or clear the attached connector's EDID. |
| `READ_CURSOR` | Include cursor pixels or receive cursor metadata/bitmaps. |
| `MANAGE_CEC` | Bind and operate the connector's userspace CEC transport. |

Starting a stream without `READ_CURSOR` requires `EXCLUDE_CURSOR`; cursor
metadata and bitmaps are also suppressed. `ATTACH_MONITOR` with an EDID
requires both attachment and EDID rights. Unknown rights are rejected.

Only one live grant with `MANAGE_ATTACHMENT` may exist per connector. Capture
remains exclusive per output. Additional capture-only grants are structurally
possible, but attachment ownership remains singular.

## Grant, attachment, and stream lifetime

The grant is durable authorization. Attachment is durable connector state
owned by the grant. A capture stream is mode-specific:

```text
grant       survives normal display reconfiguration
attachment  survives normal display reconfiguration
stream      bound to one CRTC and mode generation
buffers     bound to one stream generation and dimensions
```

Resolution, refresh, timing, EDID, blank/unblank, and CRTC reassignment never
revoke the grant. A modeset increments `mode_generation`, returns queued work
with `-ESTALE` and `MODE_CHANGED`, and requires a replacement stream and
mode-sized buffers on the same grant fd.

Detaching a monitor stops streams on that connector but does not revoke the
grant. The holder may attach it again. Detach also invalidates the CEC physical
address and cancels an outstanding CEC transmit without discarding the durable
transport binding. Attachment checks are repeated in the vblank path so detach
racing stream startup cannot disclose a later frame.

## Revocation and events

Revocation first marks the grant inert under its state lock. No new sensitive
operation can pass after that transition. Cleanup then:

1. removes and cancels grant-owned capture streams;
2. errors queued and in-flight producer fences;
3. aborts and unbinds CEC transport work;
4. detaches the grant-owned monitor and clears EDID/ELD state;
5. sends the pre-reserved `GRANT_REVOKED` event.

The reliable revoke event is reserved when the grant is created, so terminal
teardown does not allocate. Non-terminal `GRANT_STATE` events are advisory and
may coalesce or fail reservation; `GET_GRANT` is authoritative. A revoked fd
remains usable for event reads, state queries, ordinary DRM resource release,
and close, but can never regain authority.

Grant `fdinfo` reports the most recently reconciled state rather than acquiring
KMS modeset locks from a diagnostic read. Use `GET_GRANT` when an authoritative
point-in-time state query is required. That query reads connector routing under
the interruptible connection mutex rather than locking the complete modeset
object set.

Master callbacks perform only synchronous authority-state changes and queue a
worker. Stream, fence, connector, audio, and CEC cleanup occurs outside the DRM
master mutex. A master-cleanup sequence prevents a rapid transition from making
an old normal, delegated, or administrative stream usable before deferred
cleanup runs.
Deferred cleanup removes only streams whose authority generation predates the
observed cleanup sequence, so a replacement stream created after reacquisition
cannot be collected by an older work item. Temporary foreign-content states
leave the stream allocated; queue and vblank eligibility checks return or
suppress `-ESTALE` work until the composition becomes safe again.

Complete attachment and EDID transitions are serialized through their hotplug,
audio, and CEC side effects. CEC teardown blocks replacement binding until any
adapter callback that is preparing an event has retired; CEC-core callbacks do
not wait while holding the core adapter mutex. Attachment ioctls acquire the
transition lock before the holder grant lock, so detaching one connector cannot
deadlock against an EDID operation from another grant on that connector.

## PipeWire requirement

The kernel grant controls access to CastKMS pixels, not access to a PipeWire
node containing those pixels. The preferred session daemon consumes the grant
directly without publishing a generally visible source. If a separate producer
is retained, WirePlumber must restrict both video and matching audio nodes to
the intended client and destroy or revoke them with the grant.
