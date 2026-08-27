# CastKMS architecture

This is for people changing the driver. The userspace grant contract is in
[`capture-grants.md`](capture-grants.md).

## Cursor capture metadata

Capture events can report cursor state in addition to frame pixels.
`castkms_capture_cursor.c` owns cursor metadata and bitmap extraction. With
the grant's `READ_CURSOR` right, each successful event reports the cursor
position, hotspot, dimensions, visibility, and a stream-scoped image serial.

A zero serial means that no cursor state is available. A nonzero serial without
`VISIBLE` describes a hidden cursor. `IMAGE_CHANGED` tells a client to
invalidate any image cached for the previous serial.

## Source layout

The cursor capture surface has one owning file:

| Area | Owner |
|---|---|
| Captured cursor state and bitmap extraction | `castkms_capture_cursor.c` |
