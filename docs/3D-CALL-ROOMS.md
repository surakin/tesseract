# 3D call rooms (design)

Status (2026-11-25): built on Linux (Qt6) up to phase 5, not yet exercised against a real homeserver; the open points are listed at the end.
[3D-PLAN.md](3D-PLAN.md) says what is built and what is next. Companion documents: the participant data stream in
[3D-DATA-STREAMS.md](3D-DATA-STREAMS.md); the world bundle (format, publishing, consent) in [3D-WORLD-BUNDLES.md](3D-WORLD-BUNDLES.md); the sample
assets in `assets/worlds/` (with their README); the protocol draft in [3D-MSC-DRAFT.md](3D-MSC-DRAFT.md).

## Idea

A MatrixRTC call can also be seen as a shared 3D place. Participants who choose to enter it appear to each other as small robots walking
around a scene, each robot's face a screen showing that person's video. It is rendered by Tesseract's companion engine (`~/src/re`, public as
`github.com/surakin/vse`), loaded as a library. Audio and video stay the call's own; the world's data (positions) travels on a separate
stream between participants. A room can replace the built-in meadow with its own world, a bundle with scripts.

Inspired by MSC3815 ("Worlds", postponed by the Matrix Foundation), but not an implementation of it: no new room type, and the networking
MSC3815 left open is defined here on top of MatrixRTC.

## Principles

- **A regular call first.** A call always starts as a normal call with the usual grid of tiles. The world is announced and the user decides
  whether to enter it; nothing opens it automatically.
- **Nobody is left out.** Participants without the world (Element Call, other clients, a Tesseract without the engine) are in the same
  call and see and hear everyone as usual.
- **No new Matrix machinery.** No new room type, no change to how calls are set up: everything is event content on existing MatrixRTC and
  room-state mechanisms, under an unstable `dev.tesseract` prefix.
- **Encrypted where the call is.** In an encrypted call the world's data is end-to-end encrypted with the call's own keys, and unencrypted
  data is refused.
- **Shared code.** Everything except the native window plumbing lives in `ui/shared/`, as every other feature does.

## The first version

- **The scene:** a green field with trees (`assets/worlds/green_field.glb`), a rolling terrain with a daylight cycle. A clearing in the middle is
  where people appear. A room may replace it with its own world (see "Which world").
- **Avatars:** small robots (`assets/worlds/robot.glb`) whose face is a screen showing the person's camera, or their avatar picture when
  the camera is off. A name tag floats above each.
- **First person:** you see through your robot's eyes (about 1.06 m up). You never see your own robot; your self-view tile stays.
- **Walking and jumping:** the player is a physics character that stands on the ground and is stopped by tree trunks; robots are solid and
  overlapping players push each other apart. No objects, no gestures.
- **Voices** come from the robots: panned and quieter with distance.

## How a call becomes a world

### Capability

Each participant's MatrixRTC membership already lists what its client can render, in `application.capabilities` (today
`["m.render_audio", "m.render_video"]`, built in `mrtc_membership.rs`, parsed in `model.rs`, kept per member in `members.rs`). A client
that can take part in the world adds one entry:

```json
"application": {
  "type": "m.call",
  "capabilities": ["m.render_audio", "m.render_video", "dev.tesseract.world.v1"]
}
```

- The call stays an ordinary `m.call`; other clients ignore the unknown string.
- The membership is the trustworthy place: in encrypted rooms the sticky membership events are end-to-end encrypted and come from the
  member's own device, unlike LiveKit participant attributes, which the SFU sees and could alter.
- `v1` names the data stream's transport and the bundle format 1; the frames' contents belong to the world's scripts. A later version is a new
  string; a client may advertise both while versions overlap.
- The capability means "can take part", and is advertised whenever the engine is available, whichever view the user is in. It does not
  change when the user switches between grid and world, so membership events are not rewritten on every switch.
- **Who is in the world right now** comes from the data stream: a participant is in the world while their frames arrive. A participant who
  returns to the grid runs no world and sends nothing, so the others show their robot standing where it was left (idle after 3 s) until
  they leave the call.

The capability also gates the data stream: Tesseract subscribes to world data tracks, and accepts world packets, only from members that
advertise it, and does not publish its own track while no other member does.

### Which world: an optional room setting

Any room may carry an optional state event naming its world:

```json
{
  "type": "dev.tesseract.world",
  "state_key": "",
  "content": {
    "name": "Green field",
    "bundle": { "hash": "<sha-256 of the zip>", "url": "mxc://example.org/abc", "size": 397888, "format": 1 }
  }
}
```

- `bundle` is a single **source bundle**: a zip of the world's glTF files, textures, Lua scripts and a `world.json` manifest (see
  [3D-WORLD-BUNDLES.md](3D-WORLD-BUNDLES.md)). `format` is the manifest version; a client that reads only older formats shows "This
  world needs a newer Tesseract." and loads nothing.
- The **author is the event's `sender`**, which the homeserver authenticates. The content carries no author field: nothing could
  authenticate one.
- Power levels decide who may set or change it, like any room setting.
- Adding or removing it turns a room into a world room and back, with no room upgrade; existing rooms and DMs work as they are.
- Without it, Tesseract uses its built-in scene (the green field); any call offers the world, so any call can be tested.
- Sliding sync does not deliver this event, so the client reads it from the homeserver when the 3D view opens (and on Reload), not from
  sync; a change made during a call is seen by the others the next time they open or reload the view.
- Not the call's slot event (`m.rtc.slot`): that is call plumbing other clients rewrite, and an extra field there could be lost.
- A dedicated room type (as in MSC3815) can be added later on top, as a room created with this event; nothing here would change.

## Assets

- A world is one **source bundle** (a zip), published as a single media object. Every client extracts it into
  `<cache dir>/world-bundles/<hash>/` (pruned after 30 days) and the engine mints its resources locally from the glTF files, as it does
  for the built-in meadow, which is the same kind of bundle (`assets/worlds/world.json`). Paks are target-specific and cannot be edited,
  so they are not published; a pak entry could be added next to `bundle` later.
- The zip is checked on the Rust side (limits, path rules, hash) before anything is written; the event stays far below the 64 KiB limit.
- **Import cache:** the engine's minted resources of each imported glTF are kept in `<cache dir>/world-assets/<name>-<key>.twa`, so a
  later start loads them instead of importing. The key covers the file's SHA-256, the import options, the engine's import format version
  and the engine library's size and modification time, so a changed model or a new engine rebuilds the entry and prunes the old one. A
  missing, damaged or rejected entry falls back to the normal import; the cache never decides whether the world loads.
- **Downloads** go through matrix-sdk's media API (authenticated media and its cache). A downloaded bundle is verified against the event's
  hash, then extracted to `<cache dir>/world-bundles/<hash>/` (pruned after 30 days); the engine reads the world from that directory.
- **Assets are not encrypted.** State events are not end-to-end encrypted (encrypted state is experimental), so putting media keys in them
  would hand them to every homeserver in the room. A world's assets are therefore readable by the participating servers, even in an
  encrypted room. The live stream is encrypted; the scene is not.

### Uploading without duplicates

When a world is imported, the bundle is uploaded once per content hash. To avoid uploading the same bundle again for every room, a custom
account data object (unstable prefix) maps content hashes to uploads: `hash → { mxc, size }`, the scene name only as a label.

- Keyed by content hash, so a changed world is uploaded, and the same bundle imported into two rooms once.
- Before reusing an entry, Tesseract checks the media still exists on the server (homeservers purge media; the local cache would hide it)
  and uploads again if not. This happens when a world is set up or changed, which is rare.
- Account data has no compare-and-swap: re-read and merge by hash before writing. A race between two devices costs at most a duplicate
  upload.
- Each account uploads its own copies.
- Later, perhaps: reuse a matching `mxc` from the world event of any room the user is in.

## Data stream

Specified in [3D-DATA-STREAMS.md](3D-DATA-STREAMS.md). In short:

- **Transport:** a LiveKit data track named `dev.tesseract.world.v1` on the participant's primary SFU, with data packets on the same name as
  the fallback when the SFU does not support data tracks. Sending starts on packets at once and moves to the track when it is published;
  receivers accept both.
- **Multi-SFU:** send on the primary connection only, receive on every connection, accept data only from participants the existing
  `FilteredSink` gate allows on that connection, and only from members advertising the capability.
- **Encryption:** in an encrypted call, only tracks marked encrypted are subscribed, and packets from a participant whose data has not been
  encrypted are dropped.
- **Frames:** opaque to the host (at most 1 KiB); the world's scripts define them. The built-in world uses a 32-byte pose (version, kind,
  flags, sequence, sender time, position, heading, head pitch, speed) at 20 Hz while moving and 2 Hz standing, and receivers draw others
  about 100 ms behind, interpolated.

## Audio

The first version does not wait for the engine's 3D audio:

- Tesseract's mixer (`sdk/src/client/rtc/audio_mix.rs`) already receives each participant's audio separately before mixing it to one
  stream. In the world, it pans each voice left or right by its direction from the listener and lowers it with distance, using the
  positions the data stream provides.
- Outside the world (grid view) the mix is unchanged.
- **Decision 2026-11-18:** positional audio uses the engine's audio, not this Rust-mixer panning; the interim above is dropped. See [3D-PLAN.md](3D-PLAN.md),
  "Positional audio", and the engine request `tesseract-live-spatial-voices.md`.

## Video on the robots

- Remote video already reaches C++ as RGBA frames per participant (`on_rtc_video_frame`). Each frame becomes the texture of that
  participant's robot screen: the robot model's `Screen` material, a 16:9 quad with 0..1 UVs. Other aspect ratios are letterboxed.
- With the camera off, the screen shows the participant's avatar picture.
- **Quality follows distance:** a far-away robot's screen is small, so Tesseract subscribes to a lower simulcast layer for distant or
  off-screen robots, saving bandwidth and texture uploads.

## Engine integration

The engine is loaded as a library through its versioned C ABI. That ABI is being built in parallel with this design (engine Phase 23), so
this section lists what the world needs from it, not what exists at a given moment; check the engine's plan and `engine_c.h` for the
current state.

What the world needs from the engine:

- **Rendering into an off-screen surface** whose picture Tesseract reads back and draws itself, on every platform Tesseract ships, with
  resize handled.
- **Content handed in by the host:** worlds downloaded by Tesseract are checked and extracted by Tesseract, and the engine reads only that
  directory (a world's scripts and models never come from anywhere else).
- **glTF import at run time**, for the sample scenes and for worlds published as glTF.
- **One texture per participant**, updated by the host with each video frame.
- **Pointer and keyboard input** for walking and looking around, including the optional captured-mouse mode.

On Tesseract's side: the engine renders into an off-screen surface on a thread of its own, and tk draws the finished picture like any other
image (`EngineScene` produces it, `WorldView` draws it). No native child window, no subsurface: the same code runs in every shell, and
resize, focus and keyboard are tk's. Packet and frame plumbing stays in Tesseract: the engine never sees MatrixRTC or LiveKit.

**Optional at run time:** when the engine library is missing or refuses to load (wrong ABI version), Tesseract does not advertise the
capability and offers no world. Nothing else changes.

## User interface

### Announcing the world

The call starts in the grid. When anyone else in the call is in the world (their frames are arriving), a strip appears above the call
bar:

```text
 🤖  Alice and Bob are in the 3D world        [ Enter world ]   [ ✕ ]
```

- It names who is there ("Alice and Bob", "Alice and 3 others") and updates live; it disappears when nobody is in the world.
- It announces who is *in* the world, not who merely *could* be: a capable participant sitting in the grid is no reason to enter.
- **Enter world** switches the user's view. **✕** hides the strip for this call; the call bar's toggle still offers the world.
- With nobody in the world yet, the call bar's toggle still lets a capable user enter it first.

### Other surfaces

- **Lobby:** information only. One line under the camera preview ("2 people in this call are in the 3D world"); **Join** is unchanged
  and always lands in the grid.
- **Call bar:** a **Grid / World** toggle, shown while the engine is available.
- **Call banner** (the "call in progress" banner outside the call): optionally a small 3D badge. Nice to have.
- **Settings → Calls:** later, whether to show the strip at all. It changes only the announcement, never what the user joins.

### In the world

- **Where it shows:** in place of the tile grid, in the call view's DockedExpanded and Popout modes. The 220 px Docked strip and the
  Floating window are too small and keep the grid.
- **Chat stays:** in DockedExpanded the world takes the space above the composer, as video does today; Popout gives it its own window.
- **The button bar:** a handle in the top-left corner (or B) opens Import folder, Import zip and Export; it steps aside while the "world
  changed" banner shows.
- **Moving:** WASD or the arrow keys (A/D strafe, Left/Right turn). Keys move the robot only while the world has
  keyboard focus, so typing never walks. All bindings go into `shortcut_registry.h`, which also lists them in the shortcuts overlay.
- **Looking:** hold the mouse button and drag. No pointer lock by default. In Popout or full screen, an optional captured-mouse mode, left
  with Esc.
- **HUD:** the call bar stays at the bottom; the self-view tile in a corner; name tags over robots; an arrow at the screen edge with the
  name and a speaking indicator for someone outside the view.
- **Finding people:** the participant list gains **Go to**, which turns you toward someone and walks you over.
- **Others:** participants who cannot enter the world (no capability) stand at the spawn point as still robots showing their video.

### Comfort and accessibility

- Settings for turn speed and field of view, and **reduce motion** (no head bob, turning in steps), a common cure for motion sickness.
- The grid is always one click away and is the accessible view: screen readers get the existing call UI, which a 3D view cannot replace.
- Every visible string goes through `tk::tr()` with entries in every `.po` file.

## Security and privacy

- **Untrusted bundles:** a bundle comes from another user. The Rust side extracts it with limits (64 MiB, 2000 files, no `..`, absolute
  or link entries) and verifies the SHA-256 against the event; the engine's importer refuses malformed models and bounds its memory.
- **Scripts are allowed and sandboxed.** A bundle's Lua scripts run in the engine's **budgeted** mode: no `io`, `os`, `package`, `debug`,
  `ffi` or `jit`, with instruction, wall-clock and memory budgets. The sandbox limits harm, not capability: a script can draw anything,
  send frames to the room and use CPU up to its budget.
- **Consent comes before any bundle code or model import.** In order: the built-in world runs without a question; a bundle whose hash is
  in this device's trusted list runs; a bundle whose event `sender` this user has verified runs; otherwise a dialog names the sender and
  the hash and offers **Run once**, **Always run this world** (adds the hash to the trusted list, local to the device) and **Don't
  run**. Consent is per content hash, so any edit asks again. Declining, or a world that fails to download or check, switches the call
  back to the grid, never to the meadow, so users do not end up in different worlds. In a popped-out call window the question appears
  in the main window and the world area says so. A verified sender is only as trustworthy as the sender field the homeserver reports (a
  hostile homeserver could forge it); see [3D-WORLD-BUNDLES.md](3D-WORLD-BUNDLES.md), "Scripts".
- **World changes:** a new world event during a call changes nothing live. Clients in the world view get a banner ("The world changed",
  **Reload**); Reload downloads the new bundle and runs the consent check. A client in the grid loads the new world when it next opens
  World.
- **Untrusted frames:** the host bounds the size (1 KiB) and the sender (a capable member, on its own connection); the world's scripts check
  the rest (the built-in world: exact size, version and kind, finite positions clamped to the world's bounds, a rate limit per participant).
- **The SFU** sees who sends world data, how much and how often, but not its contents in an encrypted call. It can drop or delay
  frames, as it can media.
- **Assets are not secret** (see "Assets"). Users setting up a world in an encrypted room should be told the scene is readable by the
  servers.

## Phasing

1. Engine side ready: library loading, an off-screen surface drawn by tk, per-participant textures, input.
2. Capability advertised when the engine loads; the data stream (both transports) with its tests.
3. The world view with the built-in field: robots, video screens, walking, name tags, the announcement strip and the toggle.
4. Voices panned by position; distance-based video quality.
5. The room's world event, source-bundle publishing (Import and Export from the world view's button bar), and loading a room's world
   from the homeserver with the consent check. Built; see [3D-WORLD-BUNDLES.md](3D-WORLD-BUNDLES.md).

## Demo status

The call view (`CallOverlayWidget`) has a Grid / World toggle (a cube button) in the large modes, DockedExpanded and Popout, when the engine
works on this machine (a start-up probe, `EngineScene::probe`). World shows the built-in meadow with a robot per other participant, their
video on its screen; walk with WASD or the arrow keys and look by dragging. Poses travel over the call's LiveKit connection as described in
`3D-DATA-STREAMS.md`. The world view is tested on Linux (Qt6), Windows and macOS; GTK4 is untested. Everything engine-related compiles only
with `TESSERACT_WITH_ENGINE`. Phase 5 is built on Linux (Qt6) only: a world button bar (shortcut B) with Import folder, Import zip and Export, the room's
`dev.tesseract.world` event, the consent dialog and the reload banner; it has not been exercised against a real homeserver. Later stages:
name tags and the announcement strip, positional audio.

## Open questions

- Nothing on the wire names the world a frame belongs to: participants in different worlds (a missed reload) would feed their scripts frames
  they do not understand. Putting the bundle hash in the frames or the track name is a later option.
- Which SFUs support data tracks today: how often the data-packet fallback runs in practice.
- Whether participants without the capability should appear in the world at all, or only in the grid.
- If other clients ever take part, the capability, the world event, the bundle format and the data stream would need an MSC (a draft is in
  [3D-MSC-DRAFT.md](3D-MSC-DRAFT.md)); the scripts' interface would also have to be shared or replaced.
