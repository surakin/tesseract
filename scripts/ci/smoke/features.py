"""Seed a rich timeline for the smoke `features` phase (stdlib only).

The bot posts one of each kind of event the app has to render into a fresh
room the app user joins, then returns a manifest of checks. run.py writes the
manifest as JSON; the app reads it and compares each event against `expect`
(see ui/shared/app/SmokeFeatures.cpp for the supported keys). An event that
must not show in the timeline carries `{"absent": true}`.
"""

import os
import secrets
import struct
import time
import urllib.parse
import zlib

SPACING = 0.02  # seconds between posts, so origin_server_ts never ties


def _quote(part):
    return urllib.parse.quote(part, safe="!$:@")


def _png_1x1():
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    header = struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)
    pixel = zlib.compress(b"\x00\xff\x80\x00")
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", pixel) + chunk(b"IEND", b"")


class Poster:
    """Sends events as one user. Raises on any non-2xx (Server.request does)."""

    def __init__(self, server, token, sleep=time.sleep):
        self.server = server
        self.token = token
        self.sleep = sleep

    def _put(self, path, content):
        res = self.server.request("PUT", path, token=self.token, body=content)
        self.sleep(SPACING)
        return res["event_id"]

    def send(self, room, content, type="m.room.message"):
        txn = secrets.token_hex(8)
        return self._put(f"/_matrix/client/v3/rooms/{_quote(room)}/send/{type}/{txn}", content)

    def state(self, room, event_type, content, state_key=""):
        return self._put(
            f"/_matrix/client/v3/rooms/{_quote(room)}/state/{event_type}/{_quote(state_key)}",
            content,
        )

    def redact(self, room, event_id, reason):
        txn = secrets.token_hex(8)
        return self._put(
            f"/_matrix/client/v3/rooms/{_quote(room)}/redact/{_quote(event_id)}/{txn}",
            {"reason": reason},
        )


def _check(room, name, event_id, **expect):
    return {
        "name": name,
        "kind": "event",
        "room_id": room,
        "event_id": event_id,
        "expect": expect,
    }


def _throwaway_users(server, shared_secret, room, bot, nonce):
    """Registers two users and has them join `room`; returns their mxids."""
    users = []
    for role in ("kick", "ban"):
        localpart = f"smoke-{role}-{nonce}"
        mxid, token = server.register(shared_secret, localpart, secrets.token_urlsafe(18))
        bot.server.request(
            "POST",
            f"/_matrix/client/v3/rooms/{_quote(room)}/invite",
            token=bot.token,
            body={"user_id": mxid},
        )
        server.request("POST", f"/_matrix/client/v3/join/{_quote(room)}", token=token, body={})
        users.append(mxid)
    return users


def seed_features(server, state, shared_secret=None, sleep=time.sleep):
    """Seeds the features room; returns {"features_room_id", "checks"}.

    `shared_secret` is only needed for the kick and ban events, which need
    real throwaway users; without it they are skipped.
    """
    nonce = secrets.token_hex(4)
    bot_user = state["bot_user"]
    bot = Poster(server, state["bot_token"], sleep)

    room = server.request(
        "POST",
        "/_matrix/client/v3/createRoom",
        token=state["bot_token"],
        body={
            "preset": "private_chat",
            "name": f"smoke features {nonce}",
            "invite": [state["app_user"]],
        },
    )["room_id"]
    server.request(
        "POST",
        f"/_matrix/client/v3/join/{_quote(room)}",
        token=state["app_token"],
        body={},
    )

    # The app acts in this room (pins, ...): a joined member sits at power 0 and
    # m.room.pinned_events needs state_default (50). Merge into the live content
    # so the bot keeps its own power; nothing else changes.
    levels = server.request(
        "GET",
        f"/_matrix/client/v3/rooms/{_quote(room)}/state/m.room.power_levels/",
        token=state["bot_token"],
    )
    users = dict(levels.get("users", {}))
    users[state["app_user"]] = 50
    Poster(server, state["bot_token"], sleep).state(
        room, "m.room.power_levels", {**levels, "users": users}
    )

    checks = []

    def add(name, event_id, **expect):
        checks.append(_check(room, name, event_id, **expect))

    def text(body, **extra):
        return bot.send(room, {"msgtype": "m.text", "body": body, **extra})

    # Plain message, then a reply to it.
    plain_body = f"smoke plain {nonce}"
    plain = text(plain_body)
    add("plain text", plain, type="Text", sender=bot_user, body=plain_body)

    reply_body = f"smoke reply {nonce}"
    reply = text(reply_body, **{"m.relates_to": {"m.in_reply_to": {"event_id": plain}}})
    add("reply", reply, type="Text", sender=bot_user, in_reply_to_id=plain)

    # Edit keeps the original event id; the body is the new text.
    edited = text(f"smoke before edit {nonce}")
    new_body = f"smoke after edit {nonce}"
    bot.send(
        room,
        {
            "msgtype": "m.text",
            "body": "* " + new_body,
            "m.new_content": {"msgtype": "m.text", "body": new_body},
            "m.relates_to": {"rel_type": "m.replace", "event_id": edited},
        },
    )
    add("edited message", edited, type="Text", body=new_body, is_edited=True)

    # Redaction keeps the event id with type Redacted.
    doomed = text(f"smoke doomed {nonce}")
    bot.redact(room, doomed, "smoke")
    add("redacted message", doomed, type="Redacted")

    # Reaction lands on its target.
    reacted = text(f"smoke reacted {nonce}")
    bot.send(
        room,
        {"m.relates_to": {"rel_type": "m.annotation", "event_id": reacted, "key": "\U0001f44d"}},
        type="m.reaction",
    )
    add("reaction", reacted, type="Text", reactions=[{"key": "\U0001f44d", "count": 1}])

    # Thread root and a reply in the thread.
    root_body = f"smoke thread root {nonce}"
    root = text(root_body)
    thread_body = f"smoke thread reply {nonce}"
    text(
        thread_body,
        **{
            "m.relates_to": {
                "rel_type": "m.thread",
                "event_id": root,
                "is_falling_back": True,
                "m.in_reply_to": {"event_id": root},
            }
        },
    )
    # The reply itself never reaches the main timeline (it is hidden and
    # filtered before the FFI), so the root's thread summary proves it.
    add(
        "thread root",
        root,
        type="Text",
        sender=bot_user,
        body=root_body,
        thread_reply_count=1,
        thread_latest_body=thread_body,
    )

    # Other message types.
    emote_body = f"smoke emote {nonce}"
    emote = bot.send(room, {"msgtype": "m.emote", "body": emote_body})
    add("emote", emote, type="Emote", sender=bot_user, body=emote_body)
    notice_body = f"smoke notice {nonce}"
    notice = bot.send(room, {"msgtype": "m.notice", "body": notice_body})
    add("notice", notice, type="Notice", sender=bot_user, body=notice_body)

    png = _png_1x1()
    mxc = server.upload(state["bot_token"], png, "image/png", f"smoke-{nonce}.png")
    image_body = f"smoke-{nonce}.png"
    image = bot.send(
        room,
        {
            "msgtype": "m.image",
            "body": image_body,
            "url": mxc,
            "info": {"mimetype": "image/png", "w": 1, "h": 1, "size": len(png)},
        },
    )
    add("image", image, type="Image", sender=bot_user)
    file_body = f"smoke-{nonce}.txt"
    file_ev = bot.send(
        room,
        {
            "msgtype": "m.file",
            "body": file_body,
            "filename": file_body,
            "url": mxc,
            "info": {"mimetype": "image/png", "size": len(png)},
        },
    )
    add("file", file_ev, type="File", sender=bot_user)
    sticker = bot.send(
        room,
        {
            "body": f"smoke sticker {nonce}",
            "url": mxc,
            "info": {"mimetype": "image/png", "w": 1, "h": 1, "size": len(png)},
        },
        type="m.sticker",
    )
    add("sticker", sticker, type="Sticker", sender=bot_user)

    question = f"smoke poll {nonce}?"
    poll = bot.send(
        room,
        {
            "org.matrix.msc3381.poll.start": {
                "kind": "org.matrix.msc3381.poll.disclosed",
                "max_selections": 1,
                "question": {"org.matrix.msc1767.text": question},
                "answers": [
                    {"id": "yes", "org.matrix.msc1767.text": "Yes"},
                    {"id": "no", "org.matrix.msc1767.text": "No"},
                ],
            },
            "org.matrix.msc1767.text": question,
        },
        type="org.matrix.msc3381.poll.start",
    )
    add("poll", poll, type="Poll", sender=bot_user)

    location_body = f"smoke location {nonce}"
    location = bot.send(
        room,
        {"msgtype": "m.location", "body": location_body, "geo_uri": "geo:52.5200,13.4050"},
    )
    add("location", location, type="Location", sender=bot_user)

    # State events shown in the timeline.
    topic = bot.state(room, "m.room.topic", {"topic": f"smoke topic {nonce}"})
    add("room topic", topic, type="RoomTopic")
    name = bot.state(room, "m.room.name", {"name": f"smoke renamed {nonce}"})
    add("room name", name, type="RoomName")
    pinned = bot.state(room, "m.room.pinned_events", {"pinned": [plain]})
    add("pinned events", pinned, type="PinnedEvent")

    if shared_secret:
        kicked, banned = _throwaway_users(server, shared_secret, room, bot, nonce)
        kick = bot.state(
            room, "m.room.member", {"membership": "leave", "reason": "smoke kick"}, kicked
        )
        add("kick", kick, type="Membership")
        ban = bot.state(
            room, "m.room.member", {"membership": "ban", "reason": "smoke ban"}, banned
        )
        add("ban", ban, type="Membership")

    # MSC4553 forward: the original content rides in the forwarded object, the
    # top-level body is the fallback (sdk/src/msc/msc4553.rs).
    original_body = f"smoke forwarded original {nonce}"
    forward = bot.send(
        room,
        {
            "msgtype": "m.text",
            "body": f"Forwarded message:\n{original_body}",
            "m.mentions": {},
            "org.matrix.msc4553.forwarded": {
                "content": {"msgtype": "m.text", "body": original_body},
                "sender": bot_user,
                "room_id": room,
                "event_id": plain,
            },
        },
    )
    add("forwarded message", forward, type="Text", forwarded=True)

    # MSC4391 bot command: both keys, as the app sends it
    # (sdk/src/bot_commands.rs build_invocation_content).
    block = {"command": "smoke", "arguments": {}}
    command = bot.send(
        room,
        {
            "msgtype": "m.text",
            "body": f"{bot_user} smoke",
            "m.mentions": {"user_ids": [bot_user]},
            "m.bot.command": block,
            "org.matrix.msc4391.command": block,
        },
    )
    add("bot command", command, type="Text", bot_command="smoke")

    # Hidden call events: state Synapse stores but the timeline never shows.
    device = f"SMOKE{nonce}".upper()
    call_member = bot.state(
        room,
        "org.matrix.msc3401.call.member",
        {
            "m.calls": [
                {
                    "m.call_id": "",
                    "m.devices": [
                        {
                            "device_id": device,
                            "session_id": nonce,
                            "expires_ts": int(time.time() * 1000) + 3600_000,
                            "feeds": [],
                        }
                    ],
                }
            ]
        },
        f"{bot_user}",
    )
    add("hidden call member", call_member, absent=True)
    rtc_member = bot.state(
        room,
        "m.rtc.member",
        {
            "slot_id": "m.call#ROOM",
            "application": {"type": "m.call"},
            "member": {
                "id": f"{bot_user}:{device}",
                "claimed_user_id": bot_user,
                "claimed_device_id": device,
            },
            "rtc_transports": [],
            "versions": [],
        },
        f"_{bot_user}_{device}_m.call",
    )
    add("hidden rtc member", rtc_member, absent=True)

    return {"features_room_id": room, "checks": checks}


def _create_room(server, state, name, nonce, join=True, invite=True):
    """Bot creates a private room; the app user is invited and (by default) joins.

    The app only joins over REST: it never opens the room, so the room list
    has to deliver the preview and counts by itself.
    """
    body = {"preset": "private_chat", "name": f"smoke {name} {nonce}"}
    if invite:
        body["invite"] = [state["app_user"]]
    room = server.request("POST", "/_matrix/client/v3/createRoom", token=state["bot_token"], body=body)[
        "room_id"
    ]
    if join:
        server.request(
            "POST", f"/_matrix/client/v3/join/{_quote(room)}", token=state["app_token"], body={}
        )
    return room


def _room_check(room, name, **expect):
    return {"name": name, "kind": "room", "room_id": room, "expect": expect}


def seed_room_list(server, state, sleep=time.sleep):
    """Seeds rooms that exercise room-list previews, counts, tags and order.

    Returns the checks: `room` checks (compared against the room-list entry,
    the room is never opened) and one `order` check. Rooms are created oldest
    first, so `rl_old` has the earliest activity and `rl_unread` is older than
    `rl_text`: only unread-first sorting can put it ahead. The app user marks
    every room read except `rl_unread`, so no other room is unread.
    """
    nonce = secrets.token_hex(4)
    bot = Poster(server, state["bot_token"], sleep)
    app_user = state["app_user"]
    checks = []

    def mark_read(room, event_id):
        server.request(
            "POST",
            f"/_matrix/client/v3/rooms/{_quote(room)}/receipt/m.read/{_quote(event_id)}",
            token=state["app_token"],
            body={},
        )

    def text(room, body, **extra):
        return bot.send(room, {"msgtype": "m.text", "body": body, **extra})

    # Oldest room, read, so it sorts last.
    old = _create_room(server, state, "rl_old", nonce)
    mark_read(old, text(old, f"smoke old {nonce}"))

    # Three messages, one mentioning the app user: stays unread. The mention
    # carries the mxid in the body and in m.mentions, so a push rule on either
    # form fires.
    unread = _create_room(server, state, "rl_unread", nonce)
    text(unread, f"smoke unread one {nonce}")
    text(unread, f"hey {app_user} smoke mention {nonce}", **{"m.mentions": {"user_ids": [app_user]}})
    text(unread, f"smoke unread three {nonce}")
    checks.append(_room_check(unread, "rl_unread", unread_count=3, highlight_count=1))

    # Newest event is text.
    plain = _create_room(server, state, "rl_text", nonce)
    plain_body = f"smoke room text {nonce}"
    mark_read(plain, text(plain, plain_body))
    checks.append(
        _room_check(plain, "rl_text", last_message_kind="text", last_message_body=plain_body)
    )

    # An image, then a caption edit of it: the preview stays an image.
    png = _png_1x1()
    mxc = server.upload(state["bot_token"], png, "image/png", f"smoke-rl-{nonce}.png")
    edit_image = _create_room(server, state, "rl_edit_image", nonce)
    info = {"mimetype": "image/png", "w": 1, "h": 1, "size": len(png)}
    image = bot.send(
        edit_image,
        {"msgtype": "m.image", "body": f"smoke-rl-{nonce}.png", "url": mxc, "info": info},
    )
    caption = f"smoke caption {nonce}"
    edit = bot.send(
        edit_image,
        {
            "msgtype": "m.image",
            "body": "* " + caption,
            "url": mxc,
            "info": info,
            "m.new_content": {"msgtype": "m.image", "body": caption, "url": mxc, "info": info},
            "m.relates_to": {"rel_type": "m.replace", "event_id": image},
        },
    )
    mark_read(edit_image, edit)
    checks.append(_room_check(edit_image, "rl_edit_image", last_message_kind="image"))

    # A message, then a topic change: the preview stays the message.
    state_last = _create_room(server, state, "rl_state_last", nonce)
    state_body = f"smoke before topic {nonce}"
    text(state_last, state_body)
    topic = bot.state(state_last, "m.room.topic", {"topic": f"smoke room topic {nonce}"})
    mark_read(state_last, topic)
    checks.append(
        _room_check(state_last, "rl_state_last", last_message_kind="text", last_message_body=state_body)
    )

    # Invited only: the app never joins.
    invited = _create_room(server, state, "rl_invite", nonce, join=False)
    check = _room_check(invited, "rl_invite")
    check["invite"] = True
    checks.append(check)

    # Favourite through the app user's own m.tag account data.
    favourite = _create_room(server, state, "rl_favourite", nonce)
    mark_read(favourite, text(favourite, f"smoke favourite {nonce}"))
    server.request(
        "PUT",
        f"/_matrix/client/v3/user/{_quote(app_user)}/rooms/{_quote(favourite)}/tags/m.favourite",
        token=state["app_token"],
        body={"order": 0.5},
    )
    checks.append(_room_check(favourite, "rl_favourite", is_favorite=True))

    # Tombstoned toward a room the app has not joined.
    replaced = _create_room(server, state, "rl_replaced", nonce)
    successor = _create_room(server, state, "rl_successor", nonce, join=False, invite=False)
    text(replaced, f"smoke before upgrade {nonce}")
    tomb = bot.state(
        replaced,
        "m.room.tombstone",
        {"body": "This room has been replaced", "replacement_room": successor},
    )
    mark_read(replaced, tomb)
    checks.append(_room_check(replaced, "rl_replaced", replaced=True))

    checks.append(
        {
            "name": "rl_unread_text_old",
            "kind": "order",
            "room_ids": [unread, plain, old],
        }
    )
    return checks


SHRUG = "\u00af\\_(\u30c4)_/\u00af"  # what /shrug appends (SlashCommands.cpp)


def seed_app_actions(room, nonce, work_dir=None, bot_user=None):
    """Returns the `action` checks: things the app itself does in `room`.

    Each check is `{"name", "kind": "action", "op", "room_id", ...}`; the app
    performs them in this order and verify.verify_app_actions proves the
    server saw each one. `send` sends `text` through the composer (so slash
    commands go through the real dispatch) and waits for its echo to contain
    `match`; `msgtype` and `formatted_contains` are what the server event must
    carry. The other ops act on the app's own earlier send whose body is
    `target_body`, so every chain is a `send` followed by its action. A `send`
    with `verify: false` is not looked up by body; `delete` carries `after_body`
    and `before_body`, the own sends that bracket its target in the timeline.
    `upload` is seeded only with a `work_dir`: it writes a 1x1 PNG there for the
    app to attach and records the path, file name, mime type and byte size. `dm`
    is seeded only with a `bot_user`: it runs `/dm <bot_user>`, which creates a
    new direct room (nothing earlier in the run makes one).
    """

    def action(name, op, **args):
        return {"name": f"app {name}", "kind": "action", "op": op, "room_id": room, **args}

    def send(name, text, match=None, msgtype="m.text", **extra):
        return action(
            name, "send", text=text, match=match or text, msgtype=msgtype, **extra
        )

    edit_target = f"smoke app edit target {nonce}"
    delete_target = f"smoke app delete target {nonce}"
    react_target = f"smoke app react target {nonce}"
    reply_target = f"smoke app reply target {nonce}"
    pin_target = f"smoke app pin target {nonce}"
    checks = [
        send("send", f"smoke app send {nonce}"),
        send("send /me", f"/me smoke app emote {nonce}", f"smoke app emote {nonce}", "m.emote"),
        send(
            "send /shrug",
            f"/shrug smoke app shrug {nonce}",
            f"smoke app shrug {nonce} {SHRUG}",
        ),
        send(
            "send /spoiler",
            f"/spoiler smoke app spoiler {nonce}",
            f"smoke app spoiler {nonce}",
            formatted_contains="data-mx-spoiler",
        ),
        send("send (edit target)", edit_target),
        action(
            "edit",
            "edit",
            target_body=edit_target,
            new_body=f"smoke app edited {nonce}",
        ),
        # Redaction strips the body, so the server cannot be asked for this send
        # by body: the delete check below verifies it instead.
        send("send (delete target)", delete_target, verify=False),
        action(
            "delete",
            "delete",
            target_body=delete_target,
            after_body=edit_target,
            before_body=react_target,
        ),
        send("send (react target)", react_target),
        action("react", "react", target_body=react_target, key="\U0001f44d"),
        send("send (reply target)", reply_target),
        action("reply", "reply", target_body=reply_target, body=f"smoke app reply {nonce}"),
        # Appended after the chains above: the delete verifier finds its target
        # by position between two sends, so nothing may be sent between them.
        send("send (pin target)", pin_target),
        action("pin", "pin", target_body=pin_target),
        action("unpin", "unpin", target_body=pin_target),
    ]
    if work_dir:
        png = _png_1x1()
        filename = f"smoke-app-upload-{nonce}.png"
        path = os.path.join(work_dir, filename)
        with open(path, "wb") as f:
            f.write(png)
        checks.append(
            action(
                "upload",
                "upload",
                path=path,
                filename=filename,
                mime="image/png",
                size=len(png),
            )
        )
    # The room is still open here: the app's own receipt logic has to have moved
    # the read marker to the newest visible event, at or past the last message.
    checks.append(action("read receipt", "read_receipt", after_body=f"smoke app reply {nonce}"))
    # The app's create-room handler (what the Create tab's button reaches).
    checks.append(
        action(
            "create room",
            "create_room",
            room_name=f"smoke app room {nonce}",
            topic=f"smoke app topic {nonce}",
        )
    )
    if bot_user:
        # Opens (creating it) the app's direct chat with the bot.
        checks.append(
            action("dm", "dm", user=bot_user, text=f"/dm {bot_user}")
        )
    return checks
