# CastKMS architecture

This is for people changing the driver. The userspace grant contract is in
[`capture-grants.md`](capture-grants.md).

## Topology construction

`castkms_topology.c` constructs runtime DRM objects from configuration.
The default configuration can create multiple independent output
pipelines while budgeting their planes, encoders, connectors, and
optional writeback objects against DRM mask limits.

## Monitor attachment

Virtual display connectors start disconnected. A grant with
`MANAGE_ATTACHMENT` may attach its connector and optionally publish an EDID;
that transition sets the connector status, updates the standard EDID property,
and emits a KMS hotplug event. Revoking the owning grant reverses the
attachment before releasing its authority reference.

`castkms_connector_uapi.c` owns user-pointer validation and grant lookup for
monitor operations. `castkms_connector.c` owns the connector state transition.
The device attachment-transition mutex covers each complete transition, with
the attachment mutex nested inside it to protect the attached bit and owner.
Cross-authority capture cleanup runs after those transition locks are released.

## Cursor capture metadata

Capture streams include cursor planes in frame pixels by default. A stream
started with `EXCLUDE_CURSOR` omits those planes from its captured pixels.
With the grant's `READ_CURSOR` right, successful events still report cursor
position, hotspot, dimensions, visibility, and a stream-scoped image serial.
A stream without that right must exclude cursor pixels and receives no cursor
metadata.

`castkms_capture_cursor.c` owns cursor metadata and bitmap extraction. A zero
serial means that no cursor state is available. A nonzero serial without
`VISIBLE` describes a hidden cursor. `IMAGE_CHANGED` tells a client to read
the new bitmap from the event's buffer before re-queuing it and cache that
image for later events. A hidden cursor exposes no bitmap.

Writeback always retains the cursor. When writeback or frame checksums overlap
a cursor-excluded capture, frame dispatch preserves the full composition for
those consumers and renders a separate cursor-free capture result.

## Source layout

The current specialized interfaces have explicit owners:

| Area | Owner |
|---|---|
| Monitor attachment ioctl translation | `castkms_connector_uapi.c` |
| Connector attachment and EDID state | `castkms_connector.c` |
| Captured cursor state and bitmap extraction | `castkms_capture_cursor.c` |
