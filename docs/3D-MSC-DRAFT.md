# MSCXXXX: Shared 3D worlds in MatrixRTC calls

> **Internal working draft (revised 2026-11-25), not submitted.** It records what an interoperable version of Tesseract's experimental 3D call
> rooms would have to say, and follows the implementation ([3D-CALL-ROOMS.md](3D-CALL-ROOMS.md), [3D-DATA-STREAMS.md](3D-DATA-STREAMS.md),
> [3D-WORLD-BUNDLES.md](3D-WORLD-BUNDLES.md)). Since the first draft (2026-11-15) a room's world is a source bundle with scripts, not a single
> glTF file, and the data stream's format is defined by the world. It is a starting point for the author to rework, not a text to file as is.
> No MSC number is assigned.

## Introduction

[MSC3815](https://github.com/matrix-org/matrix-spec-proposals/pull/3815) ("Worlds") proposed 3D virtual worlds as Matrix rooms: a room type,
a scene in the media repository and per-member avatars. It left the real-time part, how participants see each other move, to group calls
(MSC3401), and was postponed.

Since then MatrixRTC ([MSC4143](https://github.com/matrix-org/matrix-spec-proposals/pull/4143)) and its LiveKit transport
([MSC4195](https://github.com/matrix-org/matrix-spec-proposals/pull/4195)) have become how Matrix clients hold calls. This proposal fills the
gap MSC3815 left, on top of MatrixRTC, and takes a lighter route to the rest: a 3D world is an optional *view of an ordinary call*, not a new
kind of room.

- Participants who can run a world say so in their call membership.
- A room may name a world for its calls in a state event: a **bundle** of models, scripts and a manifest in the media repository. Without
  one, clients use a default.
- Participants send each other the world's data (positions and what the world defines) over the call's transport, end-to-end encrypted when
  the call is.
- Avatars may carry a surface on which the participant's video is shown.
- A client runs a world's scripts only after the user agrees, or when the user already trusts that world or its author.

Participants without 3D support are in the same call and see an ordinary call. Nothing changes for them.

## Proposal

### 1. Capability

The MatrixRTC membership of an `m.call` application lists what the client can render in `application.capabilities` (alongside
`m.render_audio` and `m.render_video`). A client able to take part in shared 3D worlds adds:

```json
"application": {
  "type": "m.call",
  "capabilities": ["m.render_audio", "m.render_video", "m.world.v1"]
}
```

- `v1` names the version of the data stream's transport and the bundle format (sections 2 to 5). A client may list several versions.
- The capability means the client *can* take part (it can load a bundle of format 1 and run its scripts). It must not be used to say whether
  the user is currently in the 3D view: that is known from the data stream (section 5), so membership is not rewritten when a user switches
  views.
- Clients must not send world data to, or accept it from, members that do not advertise a version they share.
- Clients should not enter users into the 3D view automatically. A client may tell its user that other participants are in it and offer to
  join.

### 2. The room's world

A room may name the world its calls use, in a state event of type `m.world` with an empty state key:

```json
{
  "type": "m.world",
  "state_key": "",
  "content": {
    "name": "Meadow",
    "bundle": {
      "hash": "<64 lowercase hex characters: the SHA-256 of the bundle file>",
      "url": "mxc://example.org/AbCdEf",
      "size": 397888,
      "format": 1
    }
  }
}
```

- `bundle` is required: one zip file, described in section 3. `format` is the bundle format; a client that does not read it must refuse the
  world and say so (for example "needs a newer client"), not guess.
- `hash` is the SHA-256 of the exact bytes at `url`. Clients check `size` before downloading and the hash after, and refuse a file that
  differs. The same hash names the same world everywhere: it is what caches, trust decisions and "are we in the same world" rest on.
- **The author is the event's `sender`.** There is no author field in the content: nothing in the event could authenticate it. A replacement
  by anyone with the power to set it carries the new sender. Empty content (`{}`) removes the world.
- Without this event, a client may use a default world of its own, as a room without one is not an error. Participants in different default
  worlds still receive each other's data, so clients should prefer a room's world when one is set.
- The event is changed like any room state, under the room's power levels.
- Clients read the event from the room's current state when a user enters the 3D view, not only from sync, because it may not arrive there
  (for example with sliding sync).
- **A world change does not move anyone.** A client that sees a new event while the user is in the 3D view tells the user and lets them
  reload; reloading runs the checks of section 4. A client that does not load the new world must return that user to the ordinary call view
  and not leave them in the old world: users in the same call must not be in different worlds.

**Not encrypted.** State events are not end-to-end encrypted, so the media they reference must not be encrypted with keys carried in the
event. The bundle is readable by every homeserver in the room, even in an encrypted room. Clients setting a world in an encrypted room should
say so.

### 3. The bundle

A bundle is a zip of the world's source files with a manifest, `world.json`, at its root:

```json
{
  "format": 1,
  "name": "Meadow",
  "field_asset": "green_field.glb",
  "user_asset": "robot.glb",
  "scripts": [
    { "file": "player.lua", "attach": "local_player", "on_camera": true },
    { "file": "world.lua", "attach": "world" }
  ],
  "user_script": { "file": "robot.lua", "attach": "robot" },
  "events_out": ["pose", "send"]
}
```

- `field_asset` and `user_asset` are glTF 2.0 binaries (`.glb`): the world's scene and the avatar every participant gets. `+Y` is up, units
  are metres, the scene may contain a node named `Spawn` where participants first appear, and solid parts are marked by node name or
  `OMI_physics_*` shapes.
- `scripts` and `user_script` are Lua files the world attaches to the scene, the camera or each participant's avatar; `attach` names the
  behaviour they run as, and `params` passes numbers or strings. `events_out` names the events the host forwards to the data stream.
- Unknown manifest fields are rejected; every file the manifest names must be in the bundle.

**Limits and hostile input.** A bundle comes from another user. Clients must bound it: at most 64 MiB in total, 2000 entries, 32 MiB per file,
16 directories deep, 255 bytes per path, a manifest of at most 256 KiB, whatever sizes the zip declares. They must refuse absolute, `..`,
backslash, drive-letter, control-character, bidirectional, zero-width or otherwise invisible characters in names; Windows-reserved names;
symbolic links and other non-regular entries; encrypted entries; names that collide when compared case-insensitively; and a missing or
invalid manifest. They extract to a temporary place and use it only after the manifest passes.

**Reproducible packing.** A client that packs a directory should sort entries, fix timestamps, compression and permissions, so the same files
give the same bytes and the same hash. This makes re-publishing an unchanged world a no-op and keeps consent and caches valid.

### 4. Running a world: scripts and consent

A world is code. A bundle's scripts may draw anything, send frames to everyone in the call and use the processor, so a client must not run
them without a decision.

**Sandbox.** The runtime for scripts must be restricted: no file, process, network or native-code access, and bounds on instructions, time
and memory. The scripts' programming interface (the engine's) is out of scope here; a client that cannot run a bundle's scripts must treat
the world as unsupported (section 2).

**Consent.** Before running any script from a bundle, in this order, a client:

1. runs the scripts that came with the client itself, without asking;
2. runs a bundle whose hash the user has already trusted;
3. may run a bundle whose event `sender` the user has verified;
4. otherwise asks, naming the author and the hash, offering to run it once, to always run this world (trust the hash) or not to run it.

Consent is per hash, so any edit asks again. Nothing from the bundle, no script and no model, is loaded before the decision. Declining, or a
bundle that fails to download or check, puts the user back in the ordinary call view.

The `sender` in step 3 is vouched for only by the room's homeserver: a hostile homeserver could name a user the viewer verified as the
author. Step 3 is therefore a convenience, not authentication, and a client may leave it out.

### 5. The data stream

Each participant in the 3D view sends its own data; there is no authority other than the participant itself, and no server logic.

**Transport (LiveKit, MSC4195).** A participant publishes a LiveKit data track named `m.world.v1` on its own (publishing) SFU. If the SFU
does not support data tracks, it sends LiveKit data packets with topic `m.world.v1` instead. Receivers accept both. In multi-SFU calls, data
is sent only on the participant's own SFU, where the other members are already subscribed to its media.

**Encryption.** When the call uses end-to-end encryption (MSC4143's media keys), the data must be encrypted with the sender's current media
key, as LiveKit's data channel encryption does. Receivers in an encrypted call must drop data that was not encrypted, and must not subscribe
to a track that is not marked encrypted.

**Frames belong to the world.** The host treats each frame as opaque bytes of at most 1 KiB, received with the sender's participant id.
What a frame holds is defined by the bundle's scripts, so all participants in the same world (the same hash) understand each other, and
worlds can carry more than poses. Receivers must drop frames the world's scripts reject, and the world's scripts must treat every frame as
hostile input. A client does not interpret frames outside the scripts.

**The reference world.** Tesseract's built-in world uses two little-endian frame kinds, described in
[3D-DATA-STREAMS.md](3D-DATA-STREAMS.md): a 32-byte pose (version, kind, flags, sequence number, sender clock, position as three `f32` in
metres, heading, head pitch, speed) sent at up to 20 Hz while moving and at least 2 Hz while still, and a push frame that asks an
overlapping participant to move aside. A participant whose frames stop for 3 s is shown idle; it leaves the world when it leaves the call. A
participant in the grid view runs no world and sends none.

**Who is in the world** is known from the stream: a participant is in it while their frames arrive.

### 6. Avatars and video

The bundle's `user_asset` is the avatar every participant gets. This proposal does not define personal avatars (MSC3815's
`m.world.avatars` and `m.world.member` would serve). It defines how an avatar shows video:

- A material named `Screen` is the **video surface**. Clients replace its base color with the participant's video while their camera is on,
  and with their profile picture otherwise.
- The surface's texture coordinates span 0 to 1, with `u` running left to right as seen by someone facing the avatar, so the video is not
  mirrored to them. Clients letterbox video whose aspect ratio differs from the surface's.
- The avatar faces `+Z`, with its origin on the ground between its feet. A node may give the eye height; otherwise clients use 1.6 m.

## Potential issues

- **Scripts tie worlds to a runtime.** A bundle's Lua targets one engine's scripting interface. Another client can take part in the call and
  the data stream only if it runs the same interface, or it must fall back to the ordinary view. A later version should let the manifest name
  the runtime and its version, so a client can tell "unsupported" from "broken".
- **Mismatched worlds.** Frames are opaque to the host, so two participants in different worlds could feed each other's scripts bytes they
  do not understand. The rules of section 2 (reload, or return to the ordinary view) keep clients together, but nothing on the wire names the
  world. A later version could put the bundle hash in each frame or in the track name.
- **Consent is a human step.** Users click through warnings. The sandbox, the per-hash decision and the size bounds must limit the harm of a
  world someone agreed to run, not rely on the dialog.
- **Data tracks are new.** Many deployed SFUs may not support them yet, so the data-packet fallback will be common. Data packets in the
  current LiveKit SDKs are encrypted when the room is, but receivers are not told per packet whether a packet was encrypted, which makes the
  "drop unencrypted data" rule hard to apply exactly.
- **Worlds are public to servers.** Until encrypted state exists, bundles in encrypted rooms are not confidential.
- **Size.** A bundle is bounded at 64 MiB, which suits a room's scene, not a large streamed world. Larger worlds need an indexed form of the
  event, which this proposal does not define.
- **Different defaults.** Without a room world, participants may stand in different default worlds and see each other in different
  surroundings.

## Alternatives

- **A room type (MSC3815's `m.world`).** Fixed at room creation; an existing room or DM cannot become one without an upgrade; and it ties what
  the room is to how its calls are shown. A room type could still be added later, as a room created with a world event.
- **One glTF file as the world, no scripts.** The first draft's form: simple to render everywhere and safe by construction, but the data
  stream's format and the world's rules then have to be fixed by this proposal, and every extension needs a new MSC. Putting them in the
  world lets worlds differ, at the price of sections 3 and 4.
- **A binary format (a pak) instead of source.** Faster to load, but tied to one engine build and not editable. A bundle may later list a pak
  beside the source as an alternate.
- **The world in the call's slot event (`m.rtc.slot`).** The slot belongs to call management, which other clients and the MatrixRTC spec
  rewrite; an extra field there could be lost.
- **LiveKit participant attributes for the capability or the data.** Visible to, and changeable by, the SFU, and not end-to-end encrypted.
- **World data as Matrix events.** Far too slow and heavy for 20 Hz updates.
- **Signing bundles.** A signature by the author's key would make step 3 of section 4 real authentication; it needs a key and trust story
  this proposal does not have. Announcing the bundle hash in an encrypted message from the author, and checking the SDK's verified-sender
  state on it, is a candidate.

## Security considerations

- Bundles and avatars are untrusted input (see Potential issues): bounded before and after download, hash-checked, parsed defensively, written
  to disk only through the name rules of section 3. Exported files, which a client may offer to write for editing, are as untrusted as the
  bundle.
- Scripts run in a sandbox, only after the decision in section 4, and are the single largest risk of this proposal: they can draw anything the
  user sees, send frames to every participant and spend processor time up to their budgets.
- Data is end-to-end encrypted in encrypted calls and accepted only from MatrixRTC members whose membership names the SFU it arrives on. The
  SFU can see who sends, how much and how often, and can drop or delay data, as it can for media.
- The room's homeserver can set the `sender` it reports. A user's verification of an author is therefore only as good as their trust in
  their homeserver.
- Positions reveal where in the world a participant is to the other participants only.

## Unstable prefix

While this is a proposal, implementations use:

| Stable | Unstable |
|---|---|
| capability `m.world.v1` | `org.matrix.mscXXXX.world.v1` |
| state event `m.world` | `org.matrix.mscXXXX.world` |
| data track and topic `m.world.v1` | `org.matrix.mscXXXX.world.v1` |

Tesseract's experiment currently uses its own `dev.tesseract.world` names for the event, the capability (`dev.tesseract.world.v1`) and the
track and topic, not these.

## Dependencies

- MSC4143 (MatrixRTC) and MSC4195 (its LiveKit transport), for the call, its membership and its media keys.
- Optionally MSC3815's avatar events, for choosing avatars.
