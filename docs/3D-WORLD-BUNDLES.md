# 3D world bundles

How a room's 3D world is packaged, published and edited. Design context: [3D-CALL-ROOMS.md](3D-CALL-ROOMS.md).

## Format

A world is a zip of its source files with a manifest, `world.json`, at the root. Format 1:

```json
{
  "format": 1,
  "name": "Meadow",
  "field_asset": "green_field.glb",
  "user_asset": "robot.glb",
  "scripts": [
    { "file": "player.lua", "attach": "local_player", "on_camera": true },
    { "file": "daylight.lua", "attach": "daylight", "params": { "clock": -1 } }
  ],
  "user_script": { "file": "robot.lua", "attach": "robot" },
  "events_out": ["pose", "send"]
}
```

- `format`: the manifest version; a newer one than the client reads is refused ("needs a newer Tesseract").
- `name`: shown in the consent dialog and used as the export folder's name.
- `field_asset`, `user_asset`: glTF binaries (`.glb`) inside the bundle.
- `scripts`: Lua files with an `attach` point (the engine behavior they run as), optional `on_camera` and `params` (numbers or strings).
- `user_script`: the script every participant's avatar runs.
- `events_out`: the engine events the host forwards to the room's wire format.
- Unknown manifest fields are rejected. Every file the manifest names must exist in the bundle. Files it does not name are allowed
  (they count against the limits).

The built-in meadow is the same kind of bundle (`assets/worlds/world.json`).

## Limits and rules

Checked on the Rust side (`sdk/src/world_bundle.rs`) when importing, packing and extracting; declared sizes are never trusted.

- At most 64 MiB in total, 2000 files, 32 MiB per file, 16 directories deep, 255 bytes per path; the manifest at most 256 KiB.
- Rejected: absolute, `..`, `.`, empty, backslash, drive-letter, control-character, bidi/zero-width/invisible-filler and Windows-invalid names; symlinks and other
  non-regular entries; encrypted entries; names that collide case-insensitively; a missing or invalid manifest.
- Extraction goes to a temporary sibling directory and is renamed into place only after the manifest passes; on error nothing is left.
- Clients extract to `<cache dir>/world-bundles/<hash>/` and prune it after 30 days.

## Deterministic zip, hash

Packing a directory sorts entries by name and fixes the timestamp, compression level and permissions, so the same files always give the
same bytes. The bundle's hash is the SHA-256 of the zip, and the room event carries it: re-importing an unchanged world gives the same
hash, reuses the upload and keeps the trust decision. Downloaded bytes are verified against the event's hash and size. The zip bytes
(and so the hash) can differ across builds of the zip crate: after an upgrade the same folder may upload again and clients may ask for
consent again.

## Authoring and editing a world

1. Open a call in the room, switch to World, press **B** (or use the small handle at the left edge) to show the button bar.
2. **Export** writes the room's published world to a folder you choose (`<name>` inside it; it must not exist yet). A room without a
   world exports the built-in meadow as a starting point. Export extracts every file of a published bundle, not only the ones the manifest
   names, and skips nothing; treat exported files as untrusted (a world's author chose them) and do not open or run anything you do not know.
3. Edit the files: replace the glTF, edit the Lua scripts, adjust `world.json`.
4. **Import folder** (or **Import zip**) checks the result, uploads it once per hash and sets the room's `dev.tesseract.world` event.
   Import is disabled when you lack the power level to set room state.

Everyone in the call sees "The world changed" with **Reload**; reloading runs the consent check. Other clients ask before running your
scripts unless your account is verified by them or they already trusted the hash, so expect the dialog when you test with another
account.

## Scripts

A bundle's scripts run in the engine's budgeted sandbox (no `io`, `os`, `package`, `debug`, `ffi` or `jit`; instruction, time and memory
limits). See the engine's `docs/guide/scripting.md`. The world bundle controls behaviour and the wire format of the world data stream
(`3D-DATA-STREAMS.md`), so a script can send frames to the room: this is why consent comes first.

The warning is skipped for the built-in scripts, for a hash you already trusted, and for a sender you have verified. The sender is the
`sender` of the room's state event, which only the homeserver vouches for: a hostile homeserver could name a user you verified as the
author. "Verified sender" therefore means the account you verified, as your homeserver reports it. The sandbox above still applies.
