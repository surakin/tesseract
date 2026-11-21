#!/usr/bin/env python3
"""Server-side checks for the smoke run, independent of what the app reports.

pong_delivered: the bot sees an m.text from the app user with the run's PONG.
encrypted_pong_sent: the encrypted room holds an m.room.encrypted event from
the app user and the E2EE PONG never went out in plaintext.
app_devices: the app user's device ids (sign-in adds one, sign-out removes it).
"""

import argparse
import json
import os
import sys
import time
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smokelib import Server  # noqa: E402


def pong_delivered(server, state):
    room = urllib.parse.quote(state["room_id"], safe="")
    res = server.request(
        "GET",
        f"/_matrix/client/v3/rooms/{room}/messages?dir=b&limit=50",
        token=state["bot_token"],
    )
    for ev in res.get("chunk", []):
        c = ev.get("content", {})
        if (
            ev.get("type") == "m.room.message"
            and ev.get("sender") == state["app_user"]
            and c.get("msgtype") == "m.text"
            and c.get("body") == state["pong"]
        ):
            return True
    return False


def encrypted_pong_sent(server, state):
    room = urllib.parse.quote(state["e2ee_room_id"], safe="")
    res = server.request(
        "GET",
        f"/_matrix/client/v3/rooms/{room}/messages?dir=b&limit=50",
        token=state["bot_token"],
    )
    encrypted = False
    for ev in res.get("chunk", []):
        if ev.get("sender") != state["app_user"]:
            continue
        if ev.get("type") == "m.room.encrypted":
            encrypted = True
        elif (
            ev.get("type") == "m.room.message"
            and ev.get("content", {}).get("body") == state["e2ee_pong"]
        ):
            return False
    return encrypted


def app_devices(server, state):
    res = server.request("GET", "/_matrix/client/v3/devices", token=state["app_token"])
    return {d["device_id"] for d in res.get("devices", [])}


def _quote(part):
    return urllib.parse.quote(part, safe="")


def _room_events(server, state, room):
    res = server.request(
        "GET",
        f"/_matrix/client/v3/rooms/{_quote(room)}/messages?dir=b&limit=200",
        token=state["bot_token"],
    )
    return res.get("chunk", [])


def _own_message(events, state, body, msgtype=None):
    """The app user's m.room.message whose body is exactly `body`."""
    for ev in events:
        c = ev.get("content", {})
        if (
            ev.get("type") == "m.room.message"
            and ev.get("sender") == state["app_user"]
            and c.get("body") == body
            and (msgtype is None or c.get("msgtype") == msgtype)
        ):
            return ev
    return None


def _relations(server, state, room, event_id, rel_type):
    res = server.request(
        "GET",
        f"/_matrix/client/v1/rooms/{_quote(room)}/relations/{_quote(event_id)}/{rel_type}",
        token=state["bot_token"],
    )
    return [e for e in res.get("chunk", []) if e.get("sender") == state["app_user"]]


def _check_send(server, state, events, check):
    want_type = check.get("msgtype", "m.text")
    match = check["match"]
    saw_text = False
    for ev in events:
        c = ev.get("content", {})
        if ev.get("type") != "m.room.message" or ev.get("sender") != state["app_user"]:
            continue
        if match not in c.get("body", ""):
            continue
        saw_text = True
        if c.get("msgtype") != want_type:
            continue
        marker = check.get("formatted_contains")
        if marker and marker not in c.get("formatted_body", ""):
            continue
        return ""
    if not saw_text:
        return f"send: the server has no message from the app containing {match!r}"
    if check.get("formatted_contains"):
        return (
            f"send: the message {match!r} reached the server without "
            f"{check['formatted_contains']!r} markup or as a {want_type}"
        )
    return f"send: the message {match!r} reached the server but not as {want_type}"


def _check_edit(server, state, events, check):
    target = _own_message(events, state, check["target_body"])
    if not target:
        return f"edit: the original {check['target_body']!r} never reached the server"
    for ev in _relations(server, state, check["room_id"], target["event_id"], "m.replace"):
        if ev.get("content", {}).get("m.new_content", {}).get("body") == check["new_body"]:
            return ""
    return f"edit: no m.replace of {check['target_body']!r} with body {check['new_body']!r}"


def _check_delete(server, state, events, check):
    if _own_message(events, state, check["target_body"]):
        return f"delete: {check['target_body']!r} is still on the server with its body"
    # A redacted event has lost its body, so the target is found by position:
    # the app's own message with no body between the two sends that bracket it
    # (`events` is newest first). Limitation: the manifest cannot carry the
    # target's event id, so another own message redacted at that very spot
    # would pass too.
    after = _own_message(events, state, check["after_body"])
    before = _own_message(events, state, check["before_body"])
    if not after or not before:
        return "delete: the sends around the delete target never reached the server"
    ids = [e.get("event_id") for e in events]
    lo, hi = ids.index(before["event_id"]), ids.index(after["event_id"])
    redacted = set()
    for ev in events:
        if ev.get("type") == "m.room.redaction" and ev.get("sender") == state["app_user"]:
            victim = ev.get("redacts") or ev.get("content", {}).get("redacts")
            if victim:
                redacted.add(victim)
    stripped = [
        ev
        for ev in events[lo + 1 : hi]
        if ev.get("type") == "m.room.message"
        and ev.get("sender") == state["app_user"]
        and "body" not in ev.get("content", {})
        and ev.get("event_id") in redacted
    ]
    if len(stripped) != 1:
        return (
            f"delete: expected one redacted own message after {check['after_body']!r}, "
            f"found {len(stripped)}"
        )
    got = server.request(
        "GET",
        f"/_matrix/client/v3/rooms/{_quote(check['room_id'])}/event/{_quote(stripped[0]['event_id'])}",
        token=state["bot_token"],
    )
    if "body" in got.get("content", {}):
        return "delete: the redacted message still has a body"
    return ""


def _check_react(server, state, events, check):
    target = _own_message(events, state, check["target_body"])
    if not target:
        return f"react: the target {check['target_body']!r} never reached the server"
    for ev in _relations(server, state, check["room_id"], target["event_id"], "m.annotation"):
        if ev.get("content", {}).get("m.relates_to", {}).get("key") == check["key"]:
            return ""
    return f"react: no m.annotation {check['key']!r} from the app on {check['target_body']!r}"


def _check_reply(server, state, events, check):
    target = _own_message(events, state, check["target_body"])
    if not target:
        return f"reply: the target {check['target_body']!r} never reached the server"
    for ev in events:
        c = ev.get("content", {})
        if (
            ev.get("type") == "m.room.message"
            and ev.get("sender") == state["app_user"]
            and check["body"] in c.get("body", "")
            and c.get("m.relates_to", {}).get("m.in_reply_to", {}).get("event_id")
            == target["event_id"]
        ):
            return ""
    return f"reply: no reply to {check['target_body']!r} with body {check['body']!r}"


def _check_upload(server, state, events, check):
    name = check["filename"]
    saw = False
    for ev in events:
        c = ev.get("content", {})
        if (
            ev.get("type") != "m.room.message"
            or ev.get("sender") != state["app_user"]
            or c.get("msgtype") != "m.image"
            or name not in (c.get("body"), c.get("filename"))
        ):
            continue
        saw = True
        mxc = c.get("url", "")
        if not isinstance(mxc, str) or not mxc.startswith("mxc://") or mxc.count("/") != 3:
            continue
        origin, media_id = mxc[len("mxc://") :].split("/")
        # Authenticated media: Synapse no longer serves new uploads on v3.
        got = server.download(
            state["app_token"],
            f"/_matrix/client/v1/media/download/{_quote(origin)}/{_quote(media_id)}",
        )
        if len(got) == check["size"]:
            return ""
        return (
            f"upload: {name!r} downloads as {len(got)} bytes from {mxc}, "
            f"expected {check['size']}"
        )
    if not saw:
        return f"upload: the server has no m.image from the app named {name!r}"
    return f"upload: the m.image {name!r} has no usable mxc url"


def _check_dm(server, state, events, check):
    """`/dm <bot>` made a new two-member room the app created, listed in m.direct."""
    bot = check["user"]
    app = state["app_user"]
    try:
        direct = server.request(
            "GET",
            f"/_matrix/client/v3/user/{_quote(app)}/account_data/m.direct",
            token=state["app_token"],
        )
    except RuntimeError as e:
        return f"dm: the app has no m.direct account data ({e})"
    listed = direct.get(bot) or []
    if not listed:
        return f"dm: m.direct does not list {bot}"
    # Rooms the seed made (and every room a check names) are not the new DM.
    seeded = {state.get("room_id"), state.get("e2ee_room_id")}
    seeded.update(check.get("seeded_rooms", []))
    seeded.add(check["room_id"])
    why = []
    for room in listed:
        if room in seeded:
            why.append(f"{room} is a seeded room")
            continue
        try:
            room_state = server.request(
                "GET", f"/_matrix/client/v3/rooms/{_quote(room)}/state", token=state["app_token"]
            )
        except RuntimeError as e:
            why.append(f"{room} state unreadable ({e})")
            continue
        creator = next(
            (e.get("sender") for e in room_state if e.get("type") == "m.room.create"), None
        )
        members = {
            e.get("state_key"): e.get("content", {}).get("membership")
            for e in room_state
            if e.get("type") == "m.room.member"
        }
        # The bot is only invited: nothing in the smoke run answers the invite.
        live = {u for u, m in members.items() if m in ("join", "invite")}
        flagged = any(
            e.get("type") == "m.room.member"
            and e.get("state_key") == bot
            and e.get("content", {}).get("is_direct") is True
            for e in room_state
        )
        if creator != app:
            why.append(f"{room} was not created by the app")
        elif live != {app, bot} or members.get(app) != "join":
            why.append(f"{room} members are {sorted(live)}, expected the app and {bot}")
        elif not flagged:
            why.append(f"{room} has no is_direct flag on the bot's member event")
        else:
            return ""
    return f"dm: no new two-member room with {bot} in m.direct ({'; '.join(why)})"


def _check_read_receipt(server, state, events, check):
    """The app's public read receipt sits at or after its last own message."""
    anchor = _own_message(events, state, check["after_body"])
    if not anchor:
        return f"read_receipt: the anchor message {check['after_body']!r} never reached the server"
    room = check["room_id"]
    flt = json.dumps(
        {
            "room": {
                "rooms": [room],
                "ephemeral": {"types": ["m.receipt"]},
                "timeline": {"limit": 0},
                "state": {"types": []},
                "account_data": {"types": []},
            },
            "presence": {"types": []},
            "account_data": {"types": []},
        },
        separators=(",", ":"),
    )
    res = server.request(
        "GET",
        f"/_matrix/client/v3/sync?timeout=0&filter={urllib.parse.quote(flt, safe='')}",
        token=state["bot_token"],
    )
    ephemeral = res.get("rooms", {}).get("join", {}).get(room, {}).get("ephemeral", {})
    ids = [e.get("event_id") for e in events]  # newest first
    limit = ids.index(anchor["event_id"])
    for ev in ephemeral.get("events", []):
        if ev.get("type") != "m.receipt":
            continue
        for event_id, receipts in ev.get("content", {}).items():
            if state["app_user"] in receipts.get("m.read", {}) and event_id in ids:
                if ids.index(event_id) <= limit:
                    return ""
    return (
        f"read_receipt: no m.read receipt from the app at or after {check['after_body']!r}"
    )


def _check_create_room(server, state, events, check):
    """A room the app created exists with the requested name and topic."""
    app = state["app_user"]
    joined = server.request(
        "GET", "/_matrix/client/v3/joined_rooms", token=state["app_token"]
    ).get("joined_rooms", [])
    for room in joined:
        try:
            room_state = server.request(
                "GET", f"/_matrix/client/v3/rooms/{_quote(room)}/state", token=state["app_token"]
            )
        except RuntimeError:
            continue

        def content(kind):
            return next((e.get("content", {}) for e in room_state if e.get("type") == kind), {})

        creator = next((e.get("sender") for e in room_state if e.get("type") == "m.room.create"), None)
        if (
            creator == app
            and content("m.room.name").get("name") == check["room_name"]
            and content("m.room.topic").get("topic") == check["topic"]
        ):
            return ""
    return f"create_room: the app has created no room named {check['room_name']!r} with topic {check['topic']!r}"


def _app_pin_states(events, state):
    """The app user's m.room.pinned_events state events, oldest first."""
    return [
        ev
        for ev in reversed(events)  # `events` is newest first
        if ev.get("type") == "m.room.pinned_events"
        and ev.get("sender") == state["app_user"]
        and ev.get("state_key", "") == ""
    ]


def _pin_target(events, state, check, op):
    target = _own_message(events, state, check["target_body"])
    if not target:
        return None, f"{op}: the target {check['target_body']!r} never reached the server"
    return target["event_id"], ""


def _check_pin(server, state, events, check):
    target, reason = _pin_target(events, state, check, "pin")
    if not target:
        return reason
    for ev in _app_pin_states(events, state):
        if target in ev.get("content", {}).get("pinned", []):
            return ""
    return f"pin: no m.room.pinned_events from the app lists {check['target_body']!r}"


def _check_unpin(server, state, events, check):
    target, reason = _pin_target(events, state, check, "unpin")
    if not target:
        return reason
    pinned = False
    for ev in _app_pin_states(events, state):
        listed = target in ev.get("content", {}).get("pinned", [])
        if listed:
            pinned = True
        elif pinned:
            return ""  # a later app state event dropped it again
    if not pinned:
        return f"unpin: the app never pinned {check['target_body']!r}, so it cannot have unpinned it"
    return f"unpin: {check['target_body']!r} is still pinned (no later pinned_events without it)"


_APP_ACTION_CHECKS = {
    "send": _check_send,
    "edit": _check_edit,
    "delete": _check_delete,
    "react": _check_react,
    "reply": _check_reply,
    "upload": _check_upload,
    "dm": _check_dm,
    "create_room": _check_create_room,
    "read_receipt": _check_read_receipt,
    "pin": _check_pin,
    "unpin": _check_unpin,
}


def verify_app_actions(server, state, manifest):
    """Proves the server saw every `action` check of the features manifest.

    Returns "" or the reason of the first failing check, which names its op.
    Never raises on a missing event or a failed request.
    """
    checks = [c for c in manifest.get("checks", []) if c.get("kind") == "action"]
    if not checks:
        return "the manifest has no action checks, so nothing was verified"
    histories = {}
    for check in checks:
        op = check.get("op", "?")
        if op == "send" and check.get("verify") is False:
            continue
        checker = _APP_ACTION_CHECKS.get(op)
        if not checker:
            return f"{op}: unknown action op in check '{check.get('name')}'"
        room = check["room_id"]
        try:
            if room not in histories:
                histories[room] = _room_events(server, state, room)
            if op == "dm":
                # Every room the manifest names is one the seed made.
                check = {
                    **check,
                    "seeded_rooms": {c["room_id"] for c in manifest["checks"] if c.get("room_id")},
                }
            reason = checker(server, state, histories[room], check)
        except (RuntimeError, OSError, KeyError, ValueError) as e:
            reason = f"{op}: server check failed: {e}"
        if reason:
            return f"app action '{check.get('name')}': {reason}"
    return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ca", required=True)
    ap.add_argument("--state", required=True)
    ap.add_argument("--timeout", type=int, default=15)
    args = ap.parse_args()

    with open(args.state) as f:
        state = json.load(f)
    server = Server(state["base"], args.ca)
    deadline = time.time() + args.timeout
    while True:
        if pong_delivered(server, state):
            print(f"server saw {state['pong']} from {state['app_user']}")
            return 0
        if time.time() >= deadline:
            print(
                f"server never received {state['pong']} from {state['app_user']} "
                f"in {state['room_id']}",
                file=sys.stderr,
            )
            return 1
        time.sleep(1)


if __name__ == "__main__":
    sys.exit(main())
