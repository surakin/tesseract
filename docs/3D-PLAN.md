# 3D call world: plan

The one plan for the 3D call world (branch `3d`). Background: [3D-CALL-ROOMS.md](3D-CALL-ROOMS.md) (design and decisions),
[3D-DATA-STREAMS.md](3D-DATA-STREAMS.md) (transport and pose frame), [3D-WORLD-BUNDLES.md](3D-WORLD-BUNDLES.md) (the world bundle: format,
publishing, consent), [3D-MSC-DRAFT.md](3D-MSC-DRAFT.md) (protocol draft, written before bundles). Finished work is in CHANGES.md; this file
lists what is built (briefly), what is not yet verified, and what is open.

## State (2026-11-25)

**Built (Linux, Qt6).**

- The world view in calls and the lobby, with the built-in meadow and a robot per participant; sticky `m.rtc.member` capability and slot
  read; the pose stream with its tests ([3D-DATA-STREAMS.md](3D-DATA-STREAMS.md)).
- The engine resource cache for the `.glb` files (keyed by file hash, options, engine import format and library stamp; cleared by "Clear all
  caches").
- Participant video on the robot's `Screen` (aspect-fitted, glowing, mirroring the sky). Walking, turning, looking, jumping and the camera run
  as Lua scripts fed by input events (`assets/worlds/`); the player is an engine `character` standing on the world's static colliders; robots
  are solid and push each other apart with collision frames; a robot's head follows its player's look angle; the meadow has a daylight cycle.
- **A room's world is a source bundle** (a zip of glTF, scripts and `world.json`) named by the room's `dev.tesseract.world` state event. The
  world view has a button bar (B, or its handle in the top-left corner) with Import folder, Import zip and Export. Joiners read the event
  from the homeserver when the 3D view opens, download and check the bundle, and run its scripts in the engine's budgeted sandbox only
  after the consent check (built-in scripts, a trusted hash, a verified sender, otherwise a dialog). Declining or a failed load returns that
  client to the grid; a changed world during a call shows a Reload banner. A room without a world event shows the meadow. Format, limits
  and workflow: [3D-WORLD-BUNDLES.md](3D-WORLD-BUNDLES.md).

**Not yet verified.**

- Import, Export and the consent flow against a real homeserver, and with a second account.
- Windows, macOS and GTK builds of the bundle work (the world view itself was tested on Windows and macOS before it).
- The translations of the new strings (written by a model, not reviewed by native speakers).
- Follow-ups noted in ROADMAP.md ("3D world bar follow-ups").
- A custom world with no solid parts marked (`-col`, `-colonly`, `OMI_physics_*`) has no floor: the player falls and starts again. The host has
  no fallback floor yet (a static box at the spawn height); check this before custom worlds are shared.

**Engine.** Resource cache API, UV transform and live spatial voices (push streams, measured delay 5.33 ms) are delivered; Tesseract does not use
the voices yet. Shaders are re-minted for Windows and macOS.

## Next: per-room avatars

**Intent.** A participant may pick a personal avatar per room that replaces the world's default (`user_asset` in the manifest, the robot in the
meadow).

**Goals.** glTF binary exactly as exported (engine imports at run time); per-room avatars carried in the call membership; uploads remembered by
content hash and offered again; imported and previewed locally (importer warnings shown) before upload; optional attribution and thumbnail
per upload (field names follow MSC4032).

**Non-goals.** Building inside Tesseract; paks; formats other than `.glb`; profile-level avatars; shared avatar libraries; reusing others' uploads.

**Anti-goals.** A hostile or broken file never reaches a call (refused locally, bounded and validated again by receivers); no silent
rescaling; a failed avatar falls back to the world's default with a reason, never breaking the call; the UI never promises more privacy
than exists (in an encrypted room an avatar is readable by the room's members and your own homeserver).

**Constraints.** Engine gating, one engine per process, every engine call on the frame thread, shared code only, `tk::tr` with all `.po`
files. No new Matrix protocol beyond `dev.tesseract.*` event content; no sdk fork changes; media through matrix-sdk. The avatar and capability
ride in the sticky `m.rtc.member` only, and any new reader of sticky members goes through the open-slot check. Limit: avatar 20 MB, checked
before download and after.

**Avatar model.** One model, +Y up, metres, facing +Z, height 0.3 to 3 m (else refused). Optional material `Screen` (16:9 quad, 0..1 UVs),
animations `Walk` and `Idle`, node `Eye`. Without `Screen`, video shows on a 16:9 panel 0.15 m above the avatar, facing the viewer. Without
`Walk`/`Idle` the avatar glides. Reference walking speed 0.8 m/s. A custom avatar's `Screen` is assumed 16:9 and uses the robot's fit.

**Per-room avatar: account data plus membership.** Room account data `dev.tesseract.world.avatar` `{ hash, url, size, mimetype, file? }`
(`file` = encrypted-attachment object in encrypted rooms; absent = the default). On joining, Tesseract copies it into its membership at
`application["dev.tesseract.world.avatar"]`; changing it during a call rewrites the membership. Encrypted rooms upload with matrix-sdk's
encrypted attachments. Receivers parse it in Rust next to `capabilities`, carry it to C++ in `RtcParticipantInfo`, download through the media
cache, check size and hash, import and show it; the default stands in while it loads.

**Your uploads: deduplicated by hash.** World bundles already use the account data `dev.tesseract.world_uploads` (`hash -> { mxc, size }`,
re-checked against the media repo before reuse). Avatars should extend that one map (a `kind`, name, `encrypted`/`file?`, `uploaded_at`,
`attribution?` (up to 8 items of `{ title?, author_name, author_url?, source_url?, license? }`, each field at most 200 characters, for credit
only), and a 256 px PNG `thumbnail`), not add a second one. Read and merge before writing; a race costs at most a duplicate upload.

**UI.** Lobby / call view → My avatar in this room: earlier uploads, Import a file… (≤ 20 MB), Use the default. Import: hash the file, import in a
preview `WorldView`, show the picture, importer warnings and the avatar's height and which of `Screen`, `Walk`, `Idle`, `Eye` exist; ask for a
name, optional credit and a thumbnail; errors block publishing. The preview cannot run while the call's world view is shown.

**Loading.** `EngineScene` loads avatars from bytes through `WorldAssetLoader` (cache first; avatar height and `Screen`/`Walk`/`Idle`/`Eye` come
from the import `summary`). Imports run on the frame thread with a "Loading…" message. Two participants with the same avatar share one import.

**Security.** Size before and after download; SHA-256 checked; the engine importer bounds memory and refuses malformed files; one import at a
time per scene.

## Next: positional audio through the engine

Engine side is done (push streams). Tesseract side: each remote participant in the world view gets a push stream on their entity's audio
source (spatial, inverse model, min about 1 m, max about 25 m, Doppler off), an audio listener on the camera entity, a per-participant 10 ms
mono 48 kHz block event from Rust beside the mixed stream (not own voice), and `audio_render` called from the playback path into
`tk::AudioPlayback` while the world view shows (the mixed stream is muted meanwhile, restored when hidden). Participants without the world
capability stay in the plain mix. To measure: total voice delay (target under about 60 ms) and echo cancellation against a placed mix with real
speakers. Settings: a 3D volume control and a way back to the plain mix. Replaces the Rust-mixer interim in 3D-CALL-ROOMS.md.

## Cache follow-ups

Check `resource_mounted` before `resource_add_many` so a file shared by several participants is added once; make the cache `store()` enforce
the file-size limit and that the level id is a resource id; read the summary's `"version"` (unknown = miss); decide eviction (proposal: LRU,
512 MB, a setting); a "clear 3D cache" entry already exists via Clear all caches.

## Huge worlds and editing (discussion 2026-11-19, nothing decided)

**Why not state events.** Members receive all room state at join and an event is about 64 KB (about 400 manifest entries), so per-resource
state events or one flat manifest do not scale. The index goes into the media store as a tree.

**Shape.** `dev.tesseract.world` stays tiny: `{ root: { hash, mxc, revision, file? } }` (this would be a new format of the world event beside
today's bundle). The root names node manifests, each listing children as `{ bounds, hash, mxc, size }` (a spatial grid or octree, coarse
levels of detail near the top). Leaves are tile packs of about 1 to 20 MB. Every node is immutable and named by its content hash, so it caches on
disk forever. The client fetches the root, then only the tiles near the camera, coarse first. Small items are packed into tiles (each fetch is
an HTTP request). Receivers bound depth, node count, tile size and total bytes. Homeserver upload limits are often 50 MB, so a single-file
world cap does not hold at this scale (a bundle today is at most 64 MiB).

**Edit cost.** Editing one object uploads the changed blob, its leaf node and the nodes up to the root (3 to 4 small manifests at fan-out 64); the
cost follows tree depth, not world size. Batching edits into a commit pays it once. Orphans are a few KB of manifests per publish. State history
keeps every earlier root, so rollback is free. Alternatives weighed: manifest as one media blob (a new blob per edit), manifest inline in one
state event (size cap, last-writer-wins for the whole set), a state event per resource (state bloat, not atomic), timeline events folded on
read (every client replays history).

**Which room.** A dedicated world room (custom `type` in `m.room.create`) is preferred: edit rights differ from call rights, many call rooms can
share a world, and revision history stays out of the call. The call room carries a pointer `{ world_room, root }` like the avatar does, so
participants fetch a pinned root without joining the world room (media download needs only the `mxc`). Encrypted rooms: every blob and node is
its own attachment with its own key, so nodes grow.

**Open choices.** `mxc` inline in nodes (simple; re-uploading a purged blob changes the nodes above it, so repair is a republish) or a separate
hash-to-`mxc` table (no cascade, but needs a place that does not bloat state). Who builds the tile tree from a source scene: an offline packing
tool on the engine side with Tesseract only reading and uploading is the assumption.

**If an editor is built in the view.** Split a tile into a small scene description (entities, transforms, component values, asset hashes; edited
often) and asset blobs (`.glb`, textures; rarely). Every placeable object needs a stable id; tiles must serialize deterministically (sorted by
id) so unchanged tiles keep their hashes. Two layers: live edit operations on the data track (accepted only from senders whose power level
allows editing, as untrusted bounded input) and an explicit commit that publishes the dirty tiles up to a new root. Conflicts: per object and
property, last writer wins at first; a commit carries its base revision and merges by object id. Reserve now at no cost: a version and `format`
field on every node and tile, stable ids, and a resource backend that can write as well as read. v1 stays as designed (bundles); the editor is a
separate track.

**Engine C API gaps** (request for the engine session: `re/plans/maintenance/tesseract-streaming-resources-and-editing.md`). Streaming: a pull
provider so a load of an unmounted resource becomes pending on the host, request priority and cancel, residency reporting with a memory budget,
level despawn plus async spawn and spawn transform. Editing: stable ids, deterministic scene export, change tracking, editor drawing.

## Packaging notes

**Flatpak has no 3D room.** The manifest builds with `-DTESSERACT_WITH_ENGINE=OFF`: the engine is cloned from its own repository at build time
(`cmake/Engine.cmake`), and the Flatpak build is offline. To ship it there, stage the engine as a pinned git source in the manifest (with its
own dependencies vendored the way cargo is), point `TESSERACT_ENGINE_GIT_REPOSITORY` at that copy, and turn the option back on. Arch, deb and
rpm builds clone it from the public repository (`github.com/surakin/vse`).

**Platform-specific code in `EngineScene.cpp`.** `executable_path()` and the `installed_library()` / `installed_assets()` lookups hold
`__APPLE__`, `_WIN32` and `/proc/self/exe` branches in a shared view, against the rule that OS code lives in `ui/shared/platform/`. It should
move there as one small service (the running executable's directory), with the per-OS backends in `platform/{linux,win32,macos}/`; the engine
library and assets layout stays in `EngineScene`, or better, the host passes both paths in through `EngineScene::Options` so the view knows no
layout at all.

## Later

Building inside Tesseract (placing props); paks; profile-level avatars; shared libraries; reusing others' uploads; avatar `Screen` aspect hint;
terrain-following floors; carrying the author's identity in an encrypted message so "verified sender" no longer rests on the homeserver alone
([3D-WORLD-BUNDLES.md](3D-WORLD-BUNDLES.md), "Scripts"); a cancel token for world downloads and uploads.
