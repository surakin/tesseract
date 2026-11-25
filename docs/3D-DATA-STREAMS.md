# 3D call rooms: the participant data stream

Status (2026-11-25): built and tested on Linux (Qt6); the transport and encryption policy are done, and the frames are defined by the world's
scripts. Part of the experimental 3D call room ([3D-CALL-ROOMS.md](3D-CALL-ROOMS.md)); the sample world is in `assets/worlds/`.

## What it is for

In a 3D call room every participant is a robot walking around a shared scene. Audio and video already travel over MatrixRTC (LiveKit).
This stream carries the rest: where each robot is, which way it faces and what it is doing, so every client can draw everyone else.

Goals:

- Each client is the authority over its own robot and sends its own state; there is no server logic and no shared simulation.
- The stream rides on the call's existing LiveKit connections: no new servers, sockets or Matrix events.
- In an encrypted call the stream is end-to-end encrypted with the call's MatrixRTC keys, and unencrypted data is refused.
- It works on every LiveKit SFU, using the better transport where the SFU supports it.

Not goals (for v1): shared objects, a shared physics simulation, anything a server must arbitrate, interoperating with other clients' 3D formats, and anything
persistent (avatar choice and the world itself live in Matrix room state, not in this stream).

## Transport: data tracks, with data packets as the fallback

The pinned LiveKit Rust SDK (0.9.7) offers two ways to send application data.

**Data tracks** (`LocalParticipant::publish_data_track`, `RoomEvent::DataTrackPublished`, `RemoteDataTrack::subscribe`) are published
tracks that others subscribe to. Frames carry an optional user timestamp, frames larger than the transport MTU are split and reassembled
by the SDK, and E2EE is enforced per track: when a track is marked encrypted, a frame without encryption metadata or one that fails to
decrypt is dropped. They need a recent SFU: on an older one `publish_data_track` fails with `PublishError::Timeout` after 10 s.

**Data packets** (`LocalParticipant::publish_data`, `RoomEvent::DataReceived`) work on every SFU, lossy or reliable, with a topic. They are
encrypted automatically when the room has `RoomOptions.encryption` set (Tesseract sets it for E2EE calls), but a plaintext packet is still
delivered and the event does not say whether it was encrypted.

**Decision:** publish a data track; if publishing fails, send data packets instead. Receivers always accept both.

### Choosing per call

A client decides for its own primary connection, because that is the only SFU it publishes on (see "Multi-SFU" below):

1. When the call connects, start sending on **data packets** at once, so the robot appears without waiting.
2. In parallel, call `publish_data_track("dev.tesseract.world.v1")`.
3. If it succeeds, switch sending to the track and stop sending packets. If it times out or fails, keep the packets for the rest of the
   connection. Log which transport was chosen.
4. After a reconnect of the primary connection, repeat from step 1 (the SFU may have changed).

Both transports carry the same frame with one sequence counter (see "Frame format"), so the switch in step 3 is invisible to receivers:
they keep the newest frame per participant whichever way it came.

Some peers may use tracks and others packets, depending on their own SFUs. A receiver handles both at the same time.

### Receiving

- On `DataTrackPublished` for a track named `dev.tesseract.world.v1` from an allowed participant (below), subscribe and read frames from
  its `DataTrackStream` in a task owned by the connection. Unsubscribe on `DataTrackUnpublished`, on the participant leaving, or when the
  connection closes.
- On `DataReceived` with topic `dev.tesseract.world.v1` from an allowed participant, take the payload. Every other topic is ignored
  (Element Call and other clients may send their own).
- Both paths hand the payload to the same decoder.

## Multi-SFU

In multi-SFU calls each member publishes on its own homeserver's SFU (the primary connection), and connects subscribe-only to every other
member's SFU (`sfu_set.rs`).

- **Send only on the primary connection.** Other members are connected to it already to receive our media, so they receive our data there
  too. Sending on remote connections would duplicate it, and on a subscribe-only connection we may not be allowed to publish.
- **Receive on every connection**, primary and remote: `spawn_event_task` (`livekit_room.rs`) already runs for both and today ignores data
  events.
- **The participant gate:** data is accepted only from participants that `FilteredSink::ok()` lets through on that connection, the same
  rule as for media: a real MatrixRTC member whose membership names that SFU. So a participant on one SFU cannot pose as a member who
  publishes on another.

## Encryption policy

In an encrypted call (E2EE on for the connection):

- **Data tracks:** accept a track only if `track.info().uses_e2ee()` is true. A track that says it is unencrypted is not subscribed, and a
  warning names the participant. The SDK then drops any frame that fails to decrypt.
- **Data packets:** the SDK encrypts outgoing packets itself. For incoming ones the only signal is the participant's sticky status
  `RemoteParticipant::is_encrypted()`, which turns false for good once that participant has sent any plaintext packet. Drop packets from a
  participant whose status is false. Two known costs: it is also false for a participant who publishes no tracks at all, and a plaintext
  packet injected by the SFU silences that participant's stream for the rest of the call (the SFU can drop their packets anyway). A
  per-packet encryption flag in a later LiveKit would remove both.
- Until a participant's key arrives, their frames fail to decrypt and the SDK logs a warning per frame. Rate-limit or filter that log on
  Tesseract's side so 20 Hz of failures does not flood it.

In an unencrypted call both transports are accepted as they come.

## Frame format

The host moves opaque payloads of at most 1 KiB and knows nothing of their contents: a world's scripts encode and decode them, so a world may
define its own frames. What follows is the built-in meadow's (`assets/worlds/world.lua`), which is also the reference for the wire format
of other worlds. A world is the same for everyone in the call (same bundle hash), so everyone's scripts agree on the format; nothing on
the wire names the world.

One frame per update, binary, little-endian, 32 bytes:

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 1 | `version` | 1. A receiver drops frames of a version it does not know. |
| 1 | 1 | `kind` | 1 = pose. Other kinds are reserved; unknown kinds are dropped. |
| 2 | 2 | `flags` | Bit 0: walking. Others zero. |
| 4 | 4 | `seq` | Sender's counter, +1 per frame, shared by both transports, wrapping. |
| 8 | 4 | `time_ms` | Sender's monotonic clock in milliseconds, wrapping. |
| 12 | 12 | `position` | x, y, z as `f32`, metres, in the world's glTF frame (+Y up); y is the feet's height in the world: the ground's height plus a jump (clamped to -5..50 m). |
| 24 | 2 | `yaw` | Body heading, `u16` over a full turn. |
| 26 | 2 | `pitch` | Head pitch, `i16` over ±90°. |
| 28 | 2 | `speed` | Walking speed, `u16` in mm/s (drives the walk animation's rate). |
| 30 | 2 | reserved | Zero. |

A frame that is not exactly 32 bytes for version 1, or has a non-finite position, is dropped. Positions are clamped by the receiver to
the world's bounds.

### Collision frames (kind 2)

Every robot has a capsule body (radius 0.25 m, 2.12 m tall), so a player walking into one is stopped by it. The bodies follow the
poses each client knows, which are a little late, so two players can still overlap in each other's view. They settle it without a
server, each moving only itself: a client that sees an overlap works out how far apart the two should go, takes half of it itself
(moving away from the other) and sends the other half to the other player in a kind 2 frame. Each client averages its own half with
the halves it received and slides that far, so the physics still keeps it out of walls and other robots.

A received frame is believed only when it is plausible in the receiver's own view: the sender has a pose, is within 0.5 m of touching
and at about the same height, and the push points away from them; it is cut to the part along that direction, to half of the overlap
(with that 0.5 m of slack, as the lag between the two views) and to 0.25 m, accepted once per 100 ms from each sender, believed for
250 ms and used once. So a player cannot shove somebody they are not next to.

Same 12-byte header as a pose frame (`kind` 2, `seq` from the same counter, `time_ms`), then:

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 12 | 4 | `dx` | `f32`, metres along the world's x: how far the target should move. |
| 16 | 4 | `dz` | `f32`, along z. |
| 20 | 1 | `target_len` | Length of the target's participant id, 1 to 255. |
| 21 | n | `target` | The target's participant id, UTF-8. |

Frames go to everyone like pose frames; everyone but the target drops them. They are sent at most every 100 ms for each overlapped
player, while the overlap lasts.

### Rates

- **10–20 Hz while moving** (20 by default), lossy.
- **2 Hz while standing still**, so a newcomer sees everyone within half a second and a silent participant can be detected.
- No join snapshot is needed: the next frame is never more than 500 ms away.
- If the previous send is still in flight, the new frame replaces it rather than queueing (the same pattern as `video_in_flight`).

### On the receiving side

- Keep the newest frame per participant by `seq` (with wraparound); drop older ones, whichever transport they came on.
- Draw remote robots about 100 ms behind their newest frame, interpolating position and heading between the two frames around that time
  (sender `time_ms` gives the spacing; arrival time gives the offset).
- A participant with no frame for 3 s is shown standing at their last position; their robot goes away when they leave the call, not when
  frames stop.
- A participant who never sends frames (Element Call, or Tesseract without the 3D room) stands at the spawn point, as the room design says.

## Interfaces

**Rust (`sdk/src/client/rtc/`)**

- A small `world_stream` module owns the transport choice and the per-connection subscription tasks. It has no knowledge of the frame's
  contents: it moves opaque payloads of at most 1 KiB.
- `LiveKitRoom` gains `send_world_frame(payload)` (primary only) and starts the data-track attempt on connect. `RemoteSfuRoom` only
  receives.
- `RtcEventSink` gains `on_world_frame(session_id, participant_id, payload)`, called through `FilteredSink` like the other per-participant
  callbacks.

**FFI (`sdk/src/bridge.rs`)**

- `ClientFfi::rtc_send_world_frame(payload: &[u8])`.
- `EventHandlerBridge::on_rtc_world_frame(session_id, participant_id, payload: &[u8])`.

**C++**

- `IEventHandler` / `EventHandlerBase` forward it like `on_rtc_video_frame`, marshalled to the UI thread, ending in
  `ShellBase::handle_rtc_world_frame_ui_`.
- The C++ knows nothing of the frame. `WorldView` hands the bytes that arrive to the engine as a `frame` event and sends the bytes the
  world's script emits as `send` events (the shell's 50 ms tick drains them). Encoding and decoding, the per-participant newest-frame
  table, interpolation, send rates and the collision rules are all in `assets/worlds/world.lua`, which runs inside the engine; nothing
  runs while the 3D view is hidden, so a client in the grid view neither sends nor hears pose frames.
- Which scripts, models and host events make up the world is data, not code: `assets/worlds/world.json` (the shipped meadow bundle,
  which is the installed assets directory itself) lists the field and robot models, each script with its attach point and params, and
  the events the host listens to (`events_out`, here `pose` and `send`). `options_from_manifest` turns a manifest into
  `EngineScene::Options` and refuses paths that leave the bundle and formats newer than 1; `room_options` is that call on the meadow.
  `bundle_files` names exactly the files an export copies.

The UI-thread hop is fine at this size and rate (a few dozen participants × 20 frames × 32 bytes per second).

## Tests

- Rust: the transport choice (track succeeds, track times out, reconnect resets it), using a fake publisher; topic and name filtering;
  the `FilteredSink` gate drops data from a participant not allowed on that connection; the encryption policy (an unencrypted track is not
  subscribed, packets from a non-encrypted participant are dropped in an encrypted call); payloads over 1 KiB are refused on send.
- C++ (`tests/cpp/test_world_engine_scene.cpp`, driving `world.lua` through the engine with a test-side codec): the send rates (about
  every 500 ms standing, every 50 ms walking); a participant drawn from its frames, newest-by-`seq`; malformed frames (wrong size, unknown
  kind, NaN position) and the local echo ignored; nudge frames sent to an overlapped participant; a push believed only when it is for us
  and from a close player.
- By hand: two Tesseract clients on one SFU that supports data tracks, then on one that does not, then across two homeservers with
  different SFUs; an Element Call participant in the same call is unaffected.

## Open questions

- Which SFUs support data tracks today (the user's homeserver, matrix.org, Element's): decides how often the fallback runs in practice.
- Whether to put the world's bundle hash in each frame (or the track name), so a participant in another world, because of a missed reload,
  is ignored instead of misread. The capability itself is advertised in the membership (`dev.tesseract.world.v1`).
- The topic and track name use an unstable `dev.tesseract` prefix. If other clients ever join in, the frame format would need an MSC.
