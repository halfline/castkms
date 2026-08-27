# CastKMS architecture

This is for people changing the driver. The userspace grant contract is in
[`capture-grants.md`](capture-grants.md).

## Cursor capture metadata

Capture streams include cursor planes in frame pixels by default. A stream
started with `EXCLUDE_CURSOR` omits those planes from its captured pixels.
With the grant's `READ_CURSOR` right, successful events still report cursor
position, hotspot, dimensions, visibility, and a stream-scoped image serial.
A stream without that right must exclude cursor pixels and receives no cursor
metadata.

`castkms_capture_cursor.c` owns cursor metadata and bitmap extraction. A zero
serial means that no cursor state is available. A nonzero serial without
`VISIBLE` describes a hidden cursor. `IMAGE_CHANGED` tells a client to
invalidate any image cached for the previous serial.

Writeback always retains the cursor. When writeback or frame checksums overlap
a cursor-excluded capture, frame dispatch preserves the full composition for
those consumers and renders a separate cursor-free capture result.

## Source layout

The cursor capture surface has one owning file:

| Area | Owner |
|---|---|
| Captured cursor state and bitmap extraction | `castkms_capture_cursor.c` |
