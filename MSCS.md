# Supported MSCs & spec compatibility

Tesseract requires a homeserver that supports **Sliding Sync** natively
([MSC3575](https://github.com/matrix-org/matrix-spec-proposals/pull/3575)) —
the room list and timelines are driven entirely by it — and logs in via
OAuth 2.0 / OIDC against a **Matrix Authentication Service (MAS)** deployment,
with a legacy username/password fallback for existing accounts on servers
without MAS. See [Server requirements](../README.md#server-requirements) in the
README for details.

Beyond that baseline, individual features rely on the following Matrix Spec
Change proposals where the homeserver advertises support for them:

- [MSC2010](https://github.com/matrix-org/matrix-spec-proposals/pull/2010) — spoiler messages
- [MSC4313](https://github.com/matrix-org/matrix-spec-proposals/pull/4313) — numbered lists honour the `<ol start>` attribute
- [MSC2312](https://github.com/matrix-org/matrix-spec-proposals/pull/2312) — `matrix:` / `matrix.to` link navigation
- [MSC2545](https://github.com/matrix-org/matrix-spec-proposals/pull/2545) — custom emoji & sticker image packs
- [MSC4027](https://github.com/matrix-org/matrix-spec-proposals/pull/4027) — custom-image (emoticon) reactions
- [MSC4356](https://github.com/matrix-org/matrix-spec-proposals/pull/4356) — recently-used emoji history (also shared with Element's `io.element.recent_emoji`)
- [MSC3765](https://github.com/matrix-org/matrix-spec-proposals/pull/3765) — rich (HTML) room topics
- [MSC3952](https://github.com/matrix-org/matrix-spec-proposals/pull/3952) — intentional mentions
- [MSC2530](https://github.com/matrix-org/matrix-spec-proposals/pull/2530) — media captions
- [MSC4095](https://github.com/matrix-org/matrix-spec-proposals/pull/4095) — sender-bundled URL previews, shown directly (including in encrypted rooms), with a homeserver-generated preview as the fallback
- [MSC4452](https://github.com/matrix-org/matrix-spec-proposals/pull/4452) — the `m.preview_url` capability: when the homeserver turns link previews off, Settings says so and Tesseract stops asking it for previews
- [MSC2448](https://github.com/matrix-org/matrix-spec-proposals/pull/2448) — BlurHash media placeholders
- [MSC2705](https://github.com/matrix-org/matrix-spec-proposals/pull/2705) — animated thumbnails
- [MSC3916](https://github.com/matrix-org/matrix-spec-proposals/pull/3916) — authenticated media downloads
- [MSC4230](https://github.com/matrix-org/matrix-spec-proposals/pull/4230) — animated-image detection
- [MSC3245](https://github.com/matrix-org/matrix-spec-proposals/pull/3245) — voice messages, using the [MSC1767](https://github.com/matrix-org/matrix-spec-proposals/pull/1767) extensible-audio/waveform format
- [MSC4306](https://github.com/matrix-org/matrix-spec-proposals/pull/4306) — thread subscriptions: replying or being mentioned follows a thread, unfollowed threads show no unread, with a follow bell and a followed-only filter
- [MSC3440](https://github.com/matrix-org/matrix-spec-proposals/pull/3440) — threads, with [MSC3771](https://github.com/matrix-org/matrix-spec-proposals/pull/3771) threaded read receipts driving the per-thread unread indicator (delivered over sliding sync's [MSC3960](https://github.com/matrix-org/matrix-spec-proposals/pull/3960) receipts extension)
- [MSC3030](https://github.com/matrix-org/matrix-spec-proposals/pull/3030) — jump-to-date
- [MSC3266](https://github.com/matrix-org/matrix-spec-proposals/pull/3266) — room / space summaries
- [MSC2346](https://github.com/matrix-org/matrix-spec-proposals/pull/2346) — bridged-room detection, alongside [MSC4171](https://github.com/matrix-org/matrix-spec-proposals/pull/4171) (excludes bridge-bot ghosts from member counts)
- [MSC2403](https://github.com/matrix-org/matrix-spec-proposals/pull/2403) — room knocking: request to join a knock-restricted room, with admin/moderator review (accept / deny / deny-and-ban)
- [MSC3488](https://github.com/matrix-org/matrix-spec-proposals/pull/3488) — location messages
- [MSC3489](https://github.com/matrix-org/matrix-spec-proposals/pull/3489) — live location sharing (display only): a shared live location shows its latest position on the map and moves as updates arrive, until the share ends or times out
- [MSC3381](https://github.com/matrix-org/matrix-spec-proposals/pull/3381) — polls: shown in the timeline with live results, vote, end a poll, and create one with `/poll` *(experimental: uses the unstable `org.matrix.msc3381.poll.*` event types)*
- [MSC4274](https://github.com/matrix-org/matrix-spec-proposals/pull/4274) — inline media galleries: send up to 20 attachments as one message and browse them in the media viewer *(experimental: uses the unstable gallery event types)*
- [MSC2285](https://github.com/matrix-org/matrix-spec-proposals/pull/2285) — private read receipts
- [MSC4143](https://github.com/matrix-org/matrix-spec-proposals/pull/4143) — MatrixRTC voice/video calls, together with [MSC3401](https://github.com/matrix-org/matrix-spec-proposals/pull/3401) (call membership state), [MSC4075](https://github.com/matrix-org/matrix-spec-proposals/pull/4075) (ring notifications), [MSC4140](https://github.com/matrix-org/matrix-spec-proposals/pull/4140) (delayed events, so a crashed or disconnected client still leaves the call), [MSC4195](https://github.com/matrix-org/matrix-spec-proposals/pull/4195)/[MSC4196](https://github.com/matrix-org/matrix-spec-proposals/pull/4196)/[MSC4354](https://github.com/matrix-org/matrix-spec-proposals/pull/4354)/[MSC4519](https://github.com/matrix-org/matrix-spec-proposals/pull/4519) (transport discovery & signaling), and [MSC3417](https://github.com/matrix-org/matrix-spec-proposals/pull/3417) (dedicated call-room type: a "Call Rooms" room-list section, auto-join/auto-float/auto-restore call lifecycle tied to room navigation, and `m.intent` kept in sync with the room's declared purpose) *(creating a call room isn't exposed in the UI yet, only joining/detecting existing ones)*
- [MSC4133](https://github.com/matrix-org/matrix-spec-proposals/pull/4133) — extended profile fields: pronouns ([MSC4247](https://github.com/matrix-org/matrix-spec-proposals/pull/4247), multi-language), timezone ([MSC4175](https://github.com/matrix-org/matrix-spec-proposals/pull/4175)), and biography ([MSC4440](https://github.com/matrix-org/matrix-spec-proposals/pull/4440)); your own profile updates live when edited on another device via the [MSC4262](https://github.com/matrix-org/matrix-spec-proposals/pull/4262) sliding-sync profiles extension (Synapse: enable `include_profile_updates_in_sync`)
- [MSC4426](https://github.com/matrix-org/matrix-spec-proposals/pull/4426) — user status profile fields: a self-set emoji + text status, plus an automatic "in a call" indicator published while you're in a MatrixRTC call
- [MSC2666](https://github.com/matrix-org/matrix-spec-proposals/pull/2666) — rooms in common: a user's profile lists the rooms you share with them (needs a homeserver that supports it; the list is hidden otherwise)
- [MSC4334](https://github.com/matrix-org/matrix-spec-proposals/pull/4334) — room language: shown in the room info panel, and announced to screen readers as the language of the room's messages (Qt6, Windows and macOS)
- [MSC4108](https://github.com/matrix-org/matrix-spec-proposals/pull/4108) — QR-code login
- [MSC4153](https://github.com/matrix-org/matrix-spec-proposals/pull/4153) — "exclude insecure devices" *(opt-in setting)*: message keys go only to confirmed devices, messages from devices that aren't confirmed are hidden, and encrypted rooms warn when a member's digital identity is reset. By default (Privacy setting) a send is also refused while a member you verified has an unconfirmed device or a reset identity, and a dialog explains why and what can be done
- [MSC4405](https://github.com/matrix-org/matrix-spec-proposals/pull/4405) — SAS verification compares the `decimal` numbers first; emoji are shown only as a secondary check, and a decimal-only peer works
- [MSC4391](https://github.com/matrix-org/matrix-spec-proposals/pull/4391) — in-room bot commands
- [MSC4278](https://github.com/matrix-org/matrix-spec-proposals/pull/4278) — media preview gating
- [MSC4491](https://github.com/matrix-org/matrix-spec-proposals/pull/4491) — invite reasons on room/DM creation *(experimental; requires a homeserver advertising support, e.g. Synapse 1.156+)*
