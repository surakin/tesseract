# Smoke test: coverage and candidate features

What the CI smoke test (`.github/workflows/smoke.yml`, `scripts/ci/smoke/`,
`ui/shared/app/ShellBase_smoke.cpp`, `ui/shared/app/SmokeDirector.*`) checks
today, and a list of features it could check next, ordered by cost. This is a
planning list. Nothing below Covered now is implemented. The `features` phase
is new and has not yet run in CI.

## Covered now

- Password sign-in and sync.
- Receiving a message in a plain room.
- Sending a message in a plain room and in an E2EE room. The server must hold
  the plain one as plaintext, and the E2EE one only as `m.room.encrypted`.
- Session restore on relaunch, and sign-out. The server must drop the app's
  device.
- Legacy SSO sign-in against a fake identity provider, and a check that no login
  token appears in the app log.
- Received events (phase `features`): the bot seeds edits, redactions,
  reactions, replies, threads, emotes, notices, images, files, stickers, polls,
  locations, topic and name rows, pinned events, bot commands, forwarded
  messages and kick and ban rows, and the app must show each one. Call and RTC
  state events must stay out of the timeline.
- Room list (phase `features`): the last-message preview without opening the
  room, for plain text, an edited picture and a room whose newest event is a
  state event. Also unread and highlight counts, an invite, a favourite, a
  replaced room, and the sort order.
- Actions sent from the app, each verified on the server: send, `/me`,
  `/shrug`, `/spoiler`, edit, delete, reaction, reply, pin, unpin, file upload
  (the media repo must serve it), read receipt, room creation with name and
  topic, and `/dm` (the new DM must appear in `m.direct`).

## How the harness works

This decides what is cheap to add.

- `run.py` launches the app (built with `TESSERACT_ENABLE_SMOKE_MODE`) once per
  phase (`fresh`, `features`, `restore`, `signed-out`, `sso`) on one isolated
  profile, and stops at the first failing phase. The `features` phase relaunches
  on the stored session from `fresh` and does not sign out.
- Inside the app, `SmokeDirector` runs a list of steps. Each step is a poll on
  the UI thread, with its own timeout, that returns true when done. Steps drive
  `ShellBase` directly (open a room, fill the compose bar, read `rooms_` and
  `smoke_seen_`).
- `seed.py` registers the app user and a `bot` on a local Synapse (SQLite, no
  federation), creates one plain and one E2EE room, invites and joins the app
  user, and has the bot post a PING.
- `features.py` seeds the `features` phase: `seed_features` (the timeline
  events), `seed_room_list` (the room-list rooms) and `seed_app_actions` (the
  actions the app performs). `run.py` writes the resulting manifest to
  `<root>/features.json` and passes it as `TESSERACT_SMOKE_FEATURES_FILE`.
- Each manifest check has a `kind`. `event` waits for the named event to show up
  in the app with the expected fields, or, with `absent`, to stay unseen for a
  few seconds. `room` waits for the room-list entry to match (preview, counts,
  favourite, replaced) without opening the room; with `invite` it waits for the
  invite. `order` requires the rooms in the given order. `action` runs one op
  (`send`, `edit`, `delete`, `react`, `reply`, `pin`, `unpin`, `upload`,
  `read_receipt`, `create_room`, `dm`) through the same handlers a user reaches.
  The app-side matching is in `SmokeFeatures.cpp`, the steps in
  `ShellBase_smoke_features.cpp`.
- `verify.py` checks the result on the server over REST. `verify_app_actions`
  proves the server saw every `action` check, one checker per op.
- The offline Python tests for the harness run with
  `ctest -R smoke_scripts_unittest`.
- The bot is a plain REST client, so it cannot write into the E2EE room.

## Tier 1: remaining

Same seed, same session. The check is a REST call or the app's timeline state.
Most of Tier 1 is covered now. What is left, with the reason:

- `/myroomnick`: produces no timeline event to check.
- `/poll`, `/invite`, `/leave`, `/join`: they open dialogs or have side effects
  that would disturb later phases (`/leave` would remove the seeded room).
- Favourite, low-priority and mute toggles, and ignore and unignore a user from
  the UI: they need a settings path in the harness. A favourite room is checked
  only as a seeded state.
- Display name and avatar changes.
- Setting a room's topic, name and avatar from the app.
- Accepting and declining an invite, and the invite's room-list state after
  that.
- Space hierarchy and "Available to Join".
- Editing a picture's caption from the app, and thread replies sent from the
  app.

## Tier 2: medium

Needs new seed content or a few new harness hooks.

- Pagination: the bot sends 100+ messages, then check scroll-back, jump to date
  and jump to first unread.
- Search: query a seeded message through the local and the server-side index.
- The unread-thread indicator and the thread list.
- Push rule matching: keyword and mention highlight, and notification counts.
- Image packs and stickers (MSC2545), and `/sticker`.
- MSC4095 bundled URL previews. Needs Synapse `url_preview_enabled`.
- Room directory browsing, knocking (MSC2403), and joining by alias or link.
- Reconnect: stop and restart Synapse, then check that the app resyncs and
  re-reads server details.
- Soft logout: the server revokes the device and the app must show the login
  view.
- A wrong password gives a clear error, and a server without sliding sync gives
  the probe error from `sdk/src/oauth.rs`.
- Settings persistence: change a setting, relaunch, and check it is kept.
- History export: the produced file is valid.
- Account switching with a second account, and per-account pop-out windows.
- CLI flags (`--profile`, `--logoutall`) and single-instance forwarding.
- Clearing caches and recovering in place.

## Tier 3: heavy

Needs a second E2EE client or extra infrastructure.

- Cross-signing bootstrap, SAS verification, recovery key, key backup restore,
  silent recovery setup and identity reset. All need an E2EE-capable second
  device, such as a matrix-nio bot or a second app instance.
- Decrypting messages the bot sends into the E2EE room, which also covers
  megolm key sharing and room key requests.
- Device list, rename and sign-out of other devices (UIA).
- Calls: check that MatrixRTC member and slot events are published and the call
  indicator shows. Actually connecting needs a LiveKit server.
- A real MAS and OIDC login instead of the fake provider.
- Desktop notifications and tray, UnifiedPush, and deep-link handling.
  Platform-specific.

## Suggested next batch

The remaining Tier 1 items that need a settings path (favourite, low-priority,
mute, ignore, display name) plus accepting and declining an invite, then the
Tier 2 pagination and search items. The settings path is the one harness
addition most of them share.
