"""Offline tests for features.py: a FakeServer records every request."""

import json
import os
import sys
import unittest
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import features  # noqa: E402
import verify  # noqa: E402

BOT = "@bot-t:localhost"


class FakeServer:
    """Records (method, path, body) and answers like Synapse would."""

    def __init__(self, fail_on=None):
        self.calls = []
        self.now = 0.0
        self.fail_on = fail_on  # substring of a path that must raise
        self.n = 0

    def request(self, method, path, token=None, body=None, timeout=30):
        if self.fail_on and self.fail_on in path:
            raise RuntimeError(f"{method} {path} -> 403: forbidden")
        self.calls.append((method, path, body, self.now))
        if method == "GET" and "/state/m.room.power_levels/" in path:
            return {"users": {BOT: 100}, "state_default": 50, "events_default": 0}
        if path.endswith("/createRoom"):
            return {"room_id": "!feat:localhost"}
        self.n += 1
        return {"event_id": f"$ev{self.n}"}

    def upload(self, token, data, content_type, filename):
        self.calls.append(("POST", "/upload", data, self.now))
        return "mxc://localhost/png"

    def register(self, secret, localpart, password, admin=False):
        return f"@{localpart}:localhost", "tok-" + localpart

    def sleep(self, seconds):
        self.now += seconds


STATE = {
    "bot_user": BOT,
    "bot_token": "bt",
    "app_user": "@app:localhost",
    "app_token": "at",
    "room_id": "!main:localhost",
    "e2ee_room_id": "!e2ee:localhost",
}


def run(server=None, **kw):
    server = server or FakeServer()
    manifest = features.seed_features(server, dict(STATE), sleep=server.sleep, **kw)
    return server, manifest


def sends(server):
    return [c for c in server.calls if c[0] == "PUT"]


class SeedFeaturesTest(unittest.TestCase):
    def test_edit_posts_replace_relation_to_the_original(self):
        server, manifest = run()
        edits = [
            c for c in sends(server) if (c[2] or {}).get("m.relates_to", {}).get("rel_type") == "m.replace"
        ]
        self.assertEqual(len(edits), 1)
        target = edits[0][2]["m.relates_to"]["event_id"]
        check = next(c for c in manifest["checks"] if c["expect"].get("is_edited"))
        self.assertEqual(check["event_id"], target)
        self.assertEqual(check["expect"]["body"], edits[0][2]["m.new_content"]["body"])

    def test_redaction_targets_the_posted_event(self):
        server, manifest = run()
        redactions = [c for c in sends(server) if "/redact/" in c[1]]
        self.assertEqual(len(redactions), 1)
        check = next(c for c in manifest["checks"] if c["expect"].get("type") == "Redacted")
        self.assertIn(f"/redact/{check['event_id']}/", redactions[0][1])

    def test_every_event_in_the_manifest_has_an_event_id(self):
        _, manifest = run()
        self.assertEqual(manifest["features_room_id"], "!feat:localhost")
        self.assertTrue(manifest["checks"])
        names = set()
        for c in manifest["checks"]:
            self.assertEqual(c["kind"], "event")
            self.assertEqual(c["room_id"], "!feat:localhost")
            self.assertRegex(c["event_id"], r"^\$ev\d+$")
            self.assertIsInstance(c["expect"], dict)
            self.assertNotIn(c["name"], names)
            names.add(c["name"])

    def test_thread_summary_is_expected_on_the_root_and_no_reply_check_exists(self):
        server, manifest = run()
        by = {c["name"]: c for c in manifest["checks"]}
        self.assertNotIn("thread reply", by)
        expect = by["thread root"]["expect"]
        self.assertEqual(expect["thread_reply_count"], 1)
        replies = [
            c for c in sends(server)
            if (c[2] or {}).get("m.relates_to", {}).get("rel_type") == "m.thread"
        ]
        self.assertEqual(len(replies), 1)
        self.assertEqual(expect["thread_latest_body"], replies[0][2]["body"])

    def test_hidden_call_events_are_listed_with_expect_absent(self):
        server, manifest = run()
        absent = [c for c in manifest["checks"] if c["expect"] == {"absent": True}]
        self.assertEqual(len(absent), 2)
        types = {c[1].split("/state/")[1].split("/")[0] for c in server.calls if "/state/" in c[1]}
        self.assertIn("org.matrix.msc3401.call.member", types)
        self.assertIn("m.rtc.member", types)

    def power_levels_puts(self, server):
        return [c for c in sends(server) if "/state/m.room.power_levels/" in c[1]]

    def test_the_app_user_can_pin_before_anything_is_posted(self):
        # Setting m.room.pinned_events needs state_default (50); a joined member has 0.
        server, _ = run()
        (put,) = self.power_levels_puts(server)
        self.assertEqual(put[2]["users"][STATE["app_user"]], 50)
        self.assertEqual(put[2]["users"][BOT], 100)  # merged, the bot keeps its power
        self.assertEqual(put[2]["state_default"], 50)  # the rest of the content survives
        first_post = next(i for i, c in enumerate(server.calls) if "/send/" in c[1])
        self.assertLess(server.calls.index(put), first_post)
        join = next(
            i for i, c in enumerate(server.calls) if "/join/" in c[1] and "!feat" in c[1]
        )
        self.assertLess(join, server.calls.index(put))  # after the app joined

    def test_seed_fails_when_the_power_level_grant_is_refused(self):
        with self.assertRaises(RuntimeError):
            run(FakeServer(fail_on="/state/m.room.power_levels/"))

    def test_posts_are_spaced(self):
        server, _ = run()
        stamps = [c[3] for c in server.calls if c[0] == "PUT"]
        self.assertEqual(len(stamps), len(set(stamps)))

    def test_seeding_fails_on_a_non_2xx(self):
        with self.assertRaises(RuntimeError):
            run(FakeServer(fail_on="/redact/"))

    def test_reply_forward_and_command_expectations(self):
        _, manifest = run()
        by = {c["name"]: c for c in manifest["checks"]}
        reply = by["reply"]["expect"]
        self.assertEqual(reply["in_reply_to_id"], by["plain text"]["event_id"])
        self.assertTrue(by["forwarded message"]["expect"]["forwarded"])
        self.assertEqual(by["bot command"]["expect"]["bot_command"], "smoke")
        self.assertEqual(by["reaction"]["expect"]["reactions"], [{"key": "\U0001f44d", "count": 1}])

    def test_kick_and_ban_need_a_shared_secret(self):
        _, without = run()
        self.assertFalse([c for c in without["checks"] if c["expect"].get("type") == "Membership"])
        _, with_secret = run(shared_secret="s")
        members = [c for c in with_secret["checks"] if c["expect"].get("type") == "Membership"]
        self.assertEqual(len(members), 2)
        for c in members:
            self.assertEqual(c["expect"], {"type": "Membership"})


class RoomServer(FakeServer):
    """FakeServer with unique room ids and the token of every request."""

    def __init__(self):
        super().__init__()
        self.rooms = 0
        self.tokens = []

    def upload(self, token, data, content_type, filename):
        self.tokens.append(token)
        return super().upload(token, data, content_type, filename)

    def request(self, method, path, token=None, body=None, timeout=30):
        self.tokens.append(token)
        if path.endswith("/createRoom"):
            self.calls.append((method, path, body, self.now))
            self.rooms += 1
            return {"room_id": f"!r{self.rooms}:localhost"}
        return super().request(method, path, token, body, timeout)


def run_rooms():
    server = RoomServer()
    checks = features.seed_room_list(server, dict(STATE), sleep=server.sleep)
    return server, checks, {c["name"]: c for c in checks}


class SeedRoomListTest(unittest.TestCase):
    def test_edited_image_room_posts_edit_after_the_image(self):
        server, _, by = run_rooms()
        room = by["rl_edit_image"]["room_id"]
        posts = [c for c in sends(server) if f"/rooms/{room}/send/" in c[1]]
        self.assertEqual(len(posts), 2)
        image, edit = posts[0][2], posts[1][2]
        self.assertEqual(image["msgtype"], "m.image")
        self.assertEqual(edit["m.relates_to"]["rel_type"], "m.replace")
        self.assertEqual(edit["m.new_content"]["msgtype"], "m.image")
        self.assertEqual(by["rl_edit_image"]["expect"], {"last_message_kind": "image"})
        # The edit targets the image event (the first send in this room).
        self.assertTrue(edit["m.relates_to"]["event_id"].startswith("$ev"))
        self.assertEqual(by["rl_state_last"]["expect"]["last_message_kind"], "text")
        state_room = by["rl_state_last"]["room_id"]
        order = [c[1] for c in server.calls if state_room in c[1] and c[0] == "PUT"]
        self.assertIn("/send/", order[0])
        self.assertIn("/state/m.room.topic", order[1])

    def test_unread_room_has_one_mention_of_the_app_user(self):
        server, _, by = run_rooms()
        room = by["rl_unread"]["room_id"]
        bodies = [c[2] for c in sends(server) if f"/rooms/{room}/send/" in c[1]]
        self.assertEqual(len(bodies), 3)
        mentions = [b for b in bodies if STATE["app_user"] in b["body"]]
        self.assertEqual(len(mentions), 1)
        self.assertEqual(mentions[0]["m.mentions"], {"user_ids": [STATE["app_user"]]})
        self.assertEqual(
            by["rl_unread"]["expect"], {"unread_count": 3, "highlight_count": 1}
        )

    def test_favourite_tag_is_set_before_launch(self):
        server, _, by = run_rooms()
        room = by["rl_favourite"]["room_id"]
        want = f"/_matrix/client/v3/user/@app:localhost/rooms/{room}/tags/m.favourite"
        puts = [(i, c) for i, c in enumerate(server.calls) if c[0] == "PUT" and c[1] == want]
        self.assertEqual(len(puts), 1)
        self.assertEqual(server.tokens[puts[0][0]], "at")
        self.assertEqual(by["rl_favourite"]["expect"], {"is_favorite": True})

    def test_order_check_lists_unread_first(self):
        _, checks, by = run_rooms()
        order = by["rl_unread_text_old"]
        self.assertEqual(order["kind"], "order")
        self.assertEqual(
            order["room_ids"],
            [by["rl_unread"]["room_id"], by["rl_text"]["room_id"], "!r1:localhost"],
        )
        # The old room is created first, so its activity is the earliest.

    def test_replaced_invite_and_checks_use_supported_shapes(self):
        server, checks, by = run_rooms()
        tomb = [c for c in sends(server) if "/state/m.room.tombstone" in c[1]]
        self.assertEqual(len(tomb), 1)
        self.assertEqual(by["rl_replaced"]["expect"], {"replaced": True})
        self.assertTrue(tomb[0][2]["replacement_room"].startswith("!r"))
        self.assertNotEqual(tomb[0][2]["replacement_room"], by["rl_replaced"]["room_id"])
        self.assertTrue(by["rl_invite"]["invite"])
        self.assertEqual(by["rl_invite"]["expect"], {})
        joins = [c[1] for c in server.calls if "/join/" in c[1]]
        self.assertFalse([j for j in joins if by["rl_invite"]["room_id"] in j])
        for c in checks:
            self.assertIn(c["kind"], ("room", "order"))
        self.assertEqual(
            by["rl_text"]["expect"]["last_message_kind"], "text"
        )

    def test_posts_are_spaced_and_failures_raise(self):
        server, _, _ = run_rooms()
        stamps = [
            c[3] for c in server.calls if c[0] == "PUT" and ("/send/" in c[1] or "/state/" in c[1])
        ]
        self.assertTrue(stamps)
        self.assertEqual(len(stamps), len(set(stamps)))
        bad = RoomServer()
        bad.fail_on = "/receipt/"
        with self.assertRaises(RuntimeError):
            features.seed_room_list(bad, dict(STATE), sleep=bad.sleep)


ROOM = "!feat:localhost"
APP = "@app:localhost"
TARGETS = {
    "edit": "smoke edit target N",
    "delete": "smoke delete target N",
    "react": "smoke react target N",
    "reply": "smoke reply target N",
}


class HistoryServer:
    """Answers the three GETs verify_app_actions makes from a canned history."""

    def __init__(self, events):
        self.events = events
        self.n = 0

    def add(self, sender, type="m.room.message", **content):
        self.n += 1
        ev = {"event_id": f"$h{self.n}", "sender": sender, "type": type, "content": content}
        self.events.append(ev)
        return ev

    media = None  # {"mxc server/id": bytes}; set per test
    receipts = None  # {event_id: {"m.read": {user: {"ts": 1}}}} served in the room's /sync
    joined = None  # the app user's joined room ids
    direct = None  # the app user's m.direct content; None = no such account data
    room_state = None  # {room_id: [state events]}

    def download(self, token, path):
        """Authenticated media download only, like a Synapse that froze v3."""
        if not path.startswith("/_matrix/client/v1/media/download/"):
            raise RuntimeError(f"GET {path} -> 404: not found")
        if token != STATE["app_token"]:
            raise RuntimeError(f"GET {path} -> 401: unauthorized")
        key = path[len("/_matrix/client/v1/media/download/") :]
        if key not in (self.media or {}):
            raise RuntimeError(f"GET {path} -> 404: not found")
        return self.media[key]

    def request(self, method, path, token=None, body=None, timeout=30):
        assert method == "GET"
        if path.startswith("/_matrix/client/v3/sync?"):
            query = urllib.parse.parse_qs(path.split("?", 1)[1])
            flt = json.loads(query["filter"][0])
            if query.get("timeout") != ["0"] or "since" in query:
                raise RuntimeError("GET sync must be an immediate initial sync")
            if flt["room"].get("rooms") != [ROOM]:
                raise RuntimeError("sync filter must be limited to the room")
            if flt["room"]["ephemeral"].get("types") != ["m.receipt"]:
                raise RuntimeError("sync filter must ask for m.receipt ephemeral events")
            if token != STATE["bot_token"]:
                raise RuntimeError("sync as the wrong user")
            events = [{"type": "m.receipt", "content": self.receipts}] if self.receipts else []
            return {"rooms": {"join": {ROOM: {"ephemeral": {"events": events}}}}}
        if path == "/_matrix/client/v3/joined_rooms":
            if token != STATE["app_token"]:
                raise RuntimeError("GET joined_rooms as the wrong user")
            return {"joined_rooms": self.joined or []}
        if path.endswith("/account_data/m.direct"):
            if token != STATE["app_token"] or self.direct is None:
                raise RuntimeError(f"GET {path} -> 404: not found")
            return self.direct
        if path.endswith("/state"):
            room = urllib.parse.unquote(path.split("/rooms/")[1][: -len("/state")])
            if token != STATE["app_token"] or room not in (self.room_state or {}):
                raise RuntimeError(f"GET {path} -> 403: not in room")
            return self.room_state[room]
        if "/messages?" in path:
            return {"chunk": list(reversed(self.events))}
        if "/relations/" in path:
            if not path.startswith("/_matrix/client/v1/"):
                raise RuntimeError(f"GET {path} -> 404: unrecognised request")
            tail = path.split("/relations/")[1]
            event_id, rel = tail.split("/")[:2]
            event_id = event_id.replace("%24", "$")
            return {
                "chunk": [
                    e
                    for e in self.events
                    if e["content"].get("m.relates_to", {}).get("event_id") == event_id
                    and e["content"].get("m.relates_to", {}).get("rel_type") == rel
                ]
            }
        if "/event/" in path:
            event_id = path.split("/event/")[1].replace("%24", "$")
            for e in self.events:
                if e["event_id"] == event_id:
                    return e
            raise RuntimeError("GET -> 404")
        raise AssertionError(path)


def action_check(op, **kw):
    return {"name": f"app {op}", "kind": "action", "op": op, "room_id": ROOM, **kw}


def manifest_of(*checks):
    return {"checks": list(checks)}


def verify_one(history, check):
    server = HistoryServer(history)
    return verify.verify_app_actions(server, dict(STATE), manifest_of(check))


class VerifyAppActionsTest(unittest.TestCase):
    def test_no_action_checks_is_a_failure(self):
        reason = verify.verify_app_actions(HistoryServer([]), dict(STATE), manifest_of())
        self.assertIn("no action checks", reason)

    def test_send_plain(self):
        check = action_check("send", text="hi N", match="hi N", msgtype="m.text")
        h = HistoryServer([])
        h.add(APP, msgtype="m.text", body="hi N")
        self.assertEqual(verify_one(h.events, check), "")
        self.assertIn("send", verify_one([], check))

    def test_send_from_the_wrong_user_fails(self):
        check = action_check("send", text="hi N", match="hi N", msgtype="m.text")
        h = HistoryServer([])
        h.add(STATE["bot_user"], msgtype="m.text", body="hi N")
        self.assertIn("send", verify_one(h.events, check))

    def test_send_emote_needs_the_emote_msgtype(self):
        check = action_check("send", text="/me waves N", match="waves N", msgtype="m.emote")
        h = HistoryServer([])
        h.add(APP, msgtype="m.text", body="waves N")
        reason = verify_one(h.events, check)
        self.assertIn("send", reason)
        self.assertIn("m.emote", reason)
        h.add(APP, msgtype="m.emote", body="waves N")
        self.assertEqual(verify_one(h.events, check), "")

    def test_send_shrug_body_ends_with_the_shrug(self):
        shrug = "\u00af\\_(\u30c4)_/\u00af"
        check = action_check("send", text="/shrug s N", match="s N " + shrug, msgtype="m.text")
        h = HistoryServer([])
        h.add(APP, msgtype="m.text", body="s N")
        self.assertIn("send", verify_one(h.events, check))
        h.add(APP, msgtype="m.text", body="s N " + shrug)
        self.assertEqual(verify_one(h.events, check), "")

    def test_send_spoiler_needs_spoiler_markup(self):
        check = action_check(
            "send",
            text="/spoiler sec N",
            match="sec N",
            msgtype="m.text",
            formatted_contains="data-mx-spoiler",
        )
        h = HistoryServer([])
        h.add(APP, msgtype="m.text", body="(Spoiler) sec N")
        self.assertIn("spoiler", verify_one(h.events, check).lower())
        h.add(
            APP,
            msgtype="m.text",
            body="(Spoiler) sec N",
            format="org.matrix.custom.html",
            formatted_body="<span data-mx-spoiler>sec N</span>",
        )
        self.assertEqual(verify_one(h.events, check), "")

    def test_edit(self):
        check = action_check("edit", target_body="orig N", new_body="new N")
        h = HistoryServer([])
        target = h.add(APP, msgtype="m.text", body="orig N")
        reason = verify_one(h.events, check)
        self.assertIn("edit", reason)
        h.add(
            APP,
            msgtype="m.text",
            body="* other",
            **{
                "m.new_content": {"msgtype": "m.text", "body": "other"},
                "m.relates_to": {"rel_type": "m.replace", "event_id": target["event_id"]},
            },
        )
        self.assertIn("edit", verify_one(h.events, check))
        h.add(
            APP,
            msgtype="m.text",
            body="* new N",
            **{
                "m.new_content": {"msgtype": "m.text", "body": "new N"},
                "m.relates_to": {"rel_type": "m.replace", "event_id": target["event_id"]},
            },
        )
        self.assertEqual(verify_one(h.events, check), "")

    def test_edit_missing_target_names_the_op(self):
        check = action_check("edit", target_body="orig N", new_body="new N")
        self.assertIn("edit", verify_one([], check))

    def delete_history(self):
        h = HistoryServer([])
        h.add(APP, msgtype="m.text", body="before N")
        target = h.add(APP, msgtype="m.text", body="doomed N")
        h.add(APP, msgtype="m.text", body="after N")
        return h, target

    def delete_check(self):
        return action_check(
            "delete", target_body="doomed N", after_body="before N", before_body="after N"
        )

    def test_delete(self):
        check = self.delete_check()
        h, target = self.delete_history()
        self.assertIn("delete", verify_one(h.events, check))
        target["content"] = {}
        h.add(APP, type="m.room.redaction", reason="x")["redacts"] = target["event_id"]
        self.assertEqual(verify_one(h.events, check), "")

    def test_delete_of_an_unredacted_event_fails(self):
        check = self.delete_check()
        h, target = self.delete_history()
        other = h.events[0]
        other["content"] = {}
        h.add(APP, type="m.room.redaction")["redacts"] = other["event_id"]
        self.assertIn("delete", verify_one(h.events, check))

    def test_delete_redaction_by_another_user_does_not_count(self):
        check = self.delete_check()
        h, target = self.delete_history()
        target["content"] = {}
        h.add(STATE["bot_user"], type="m.room.redaction")["redacts"] = target["event_id"]
        self.assertIn("delete", verify_one(h.events, check))

    def test_react(self):
        check = action_check("react", target_body="rt N", key="\U0001f44d")
        h = HistoryServer([])
        target = h.add(APP, msgtype="m.text", body="rt N")
        self.assertIn("react", verify_one(h.events, check))
        h.add(
            APP,
            type="m.reaction",
            **{"m.relates_to": {"rel_type": "m.annotation", "event_id": target["event_id"], "key": "x"}},
        )
        self.assertIn("react", verify_one(h.events, check))
        h.add(
            APP,
            type="m.reaction",
            **{
                "m.relates_to": {
                    "rel_type": "m.annotation",
                    "event_id": target["event_id"],
                    "key": "\U0001f44d",
                }
            },
        )
        self.assertEqual(verify_one(h.events, check), "")

    def test_reply(self):
        check = action_check("reply", target_body="rt N", body="answer N")
        h = HistoryServer([])
        target = h.add(APP, msgtype="m.text", body="rt N")
        self.assertIn("reply", verify_one(h.events, check))
        h.add(APP, msgtype="m.text", body="answer N")  # sent, but not as a reply
        self.assertIn("reply", verify_one(h.events, check))
        h.add(
            APP,
            msgtype="m.text",
            body="answer N",
            **{"m.relates_to": {"m.in_reply_to": {"event_id": target["event_id"]}}},
        )
        self.assertEqual(verify_one(h.events, check), "")

    def pin_state(self, h, *event_ids, sender=APP):
        return h.add(sender, type="m.room.pinned_events", pinned=list(event_ids))

    def test_pin(self):
        check = action_check("pin", target_body="pin N")
        h = HistoryServer([])
        target = h.add(APP, msgtype="m.text", body="pin N")
        self.assertIn("pin", verify_one(h.events, check))
        self.pin_state(h, "$other")  # a pin of something else
        self.assertIn("pin", verify_one(h.events, check))
        self.pin_state(h, target["event_id"], sender=STATE["bot_user"])  # not the app
        self.assertIn("pin", verify_one(h.events, check))
        self.pin_state(h, "$other", target["event_id"])
        self.assertEqual(verify_one(h.events, check), "")

    def test_pin_missing_target_names_the_op(self):
        self.assertIn("pin", verify_one([], action_check("pin", target_body="pin N")))

    def test_unpin_needs_a_later_state_without_the_target(self):
        check = action_check("unpin", target_body="pin N")
        h = HistoryServer([])
        target = h.add(APP, msgtype="m.text", body="pin N")
        self.assertIn("unpin", verify_one(h.events, check))
        self.pin_state(h, "$unrelated")  # never pinned the target: not an unpin
        self.assertIn("unpin", verify_one(h.events, check))
        self.pin_state(h, target["event_id"])
        self.assertIn("unpin", verify_one(h.events, check))  # still pinned
        self.pin_state(h, "$unrelated")
        self.assertEqual(verify_one(h.events, check), "")

    def test_unpin_before_the_pin_does_not_count(self):
        check = action_check("unpin", target_body="pin N")
        h = HistoryServer([])
        target = h.add(APP, msgtype="m.text", body="pin N")
        self.pin_state(h, "$unrelated")
        self.pin_state(h, target["event_id"])
        self.assertIn("unpin", verify_one(h.events, check))

    def upload_check(self, **kw):
        args = dict(
            path="/work/smoke-upload.png", filename="smoke-upload-N.png", mime="image/png", size=4
        )
        args.update(kw)
        return action_check("upload", **args)

    def verify_upload(self, h, check, media=None):
        h.media = media
        return verify.verify_app_actions(h, dict(STATE), manifest_of(check))

    def test_upload(self):
        check = self.upload_check()
        h = HistoryServer([])
        self.assertIn("upload", self.verify_upload(h, check))
        h.add(APP, msgtype="m.image", body="smoke-upload-N.png", url="mxc://localhost/abc")
        self.assertIn("upload", self.verify_upload(h, check))  # not downloadable
        self.assertIn("upload", self.verify_upload(h, check, {"localhost/abc": b"123"}))  # size
        self.assertEqual(self.verify_upload(h, check, {"localhost/abc": b"1234"}), "")

    def test_upload_needs_an_image_event_from_the_app_with_a_url(self):
        check = self.upload_check()
        media = {"localhost/abc": b"1234"}
        h = HistoryServer([])
        h.add(STATE["bot_user"], msgtype="m.image", body="smoke-upload-N.png", url="mxc://localhost/abc")
        self.assertIn("upload", self.verify_upload(h, check, media))  # wrong sender
        h.add(APP, msgtype="m.file", body="smoke-upload-N.png", url="mxc://localhost/abc")
        self.assertIn("upload", self.verify_upload(h, check, media))  # wrong msgtype
        h.add(APP, msgtype="m.image", body="smoke-upload-N.png")
        self.assertIn("upload", self.verify_upload(h, check, media))  # no url
        h.add(APP, msgtype="m.image", body="smoke-upload-N.png", url="http://x/y")
        self.assertIn("upload", self.verify_upload(h, check, media))  # not an mxc
        h.add(APP, msgtype="m.image", body="other.png", url="mxc://localhost/abc")
        self.assertIn("upload", self.verify_upload(h, check, media))  # other file

    def test_upload_downloads_through_the_authenticated_v1_path(self):
        calls = []

        class Spy(HistoryServer):
            def download(self, token, path):
                calls.append((token, path))
                return super().download(token, path)

        h = Spy([])
        h.add(APP, msgtype="m.image", body="smoke-upload-N.png", url="mxc://localhost/abc")
        h.media = {"localhost/abc": b"1234"}
        reason = verify.verify_app_actions(h, dict(STATE), manifest_of(self.upload_check()))
        self.assertEqual(reason, "")
        self.assertEqual(calls, [("at", "/_matrix/client/v1/media/download/localhost/abc")])

    DM = "!dm:localhost"

    def dm_state(self, creator=APP, extra_members=(), bot_membership="invite"):
        bot = STATE["bot_user"]
        members = [(APP, "join"), (bot, bot_membership)] + [(m, "join") for m in extra_members]
        return [
            {"type": "m.room.create", "state_key": "", "sender": creator, "content": {}},
        ] + [
            {
                "type": "m.room.member",
                "state_key": user,
                "sender": user if m == "join" else creator,
                "content": {"membership": m, "is_direct": True} if user == bot else {"membership": m},
            }
            for user, m in members
        ]

    def verify_dm(self, direct, room_state):
        h = HistoryServer([])
        h.direct, h.room_state = direct, room_state
        check = action_check("dm", user=STATE["bot_user"])
        return verify.verify_app_actions(h, dict(STATE), manifest_of(check))

    def test_dm(self):
        bot = STATE["bot_user"]
        self.assertIn("dm", self.verify_dm(None, {}))  # no m.direct at all
        self.assertIn("dm", self.verify_dm({}, {}))
        self.assertEqual(self.verify_dm({bot: [self.DM]}, {self.DM: self.dm_state()}), "")
        self.assertEqual(
            self.verify_dm({bot: [self.DM]}, {self.DM: self.dm_state(bot_membership="join")}), ""
        )

    def test_dm_does_not_count_seeded_rooms(self):
        bot = STATE["bot_user"]
        for seeded in (STATE["room_id"], STATE["e2ee_room_id"], ROOM):
            reason = self.verify_dm({bot: [seeded]}, {seeded: self.dm_state()})
            self.assertIn("dm", reason, seeded)

    def test_dm_needs_the_bot_key_and_a_two_member_room_the_app_created(self):
        bot = STATE["bot_user"]
        other = "@other:localhost"
        self.assertIn("dm", self.verify_dm({other: [self.DM]}, {self.DM: self.dm_state()}))
        self.assertIn(
            "dm", self.verify_dm({bot: [self.DM]}, {self.DM: self.dm_state(creator=bot)})
        )
        self.assertIn(
            "dm",
            self.verify_dm({bot: [self.DM]}, {self.DM: self.dm_state(extra_members=[other])}),
        )
        left = self.dm_state(bot_membership="leave")
        self.assertIn("dm", self.verify_dm({bot: [self.DM]}, {self.DM: left}))

    def test_dm_needs_is_direct_on_the_bots_member_event(self):
        bot = STATE["bot_user"]
        state = self.dm_state()
        for ev in state:
            ev["content"].pop("is_direct", None)
        reason = self.verify_dm({bot: [self.DM]}, {self.DM: state})
        self.assertIn("dm", reason)
        self.assertIn("is_direct", reason)

    def test_dm_unreadable_state_is_a_reason(self):
        bot = STATE["bot_user"]
        self.assertIn("dm", self.verify_dm({bot: [self.DM]}, {}))

    def receipt_history(self):
        h = HistoryServer([])
        older = h.add(STATE["bot_user"], msgtype="m.text", body="older N")
        after = h.add(APP, msgtype="m.text", body="after N")
        newer = h.add(STATE["bot_user"], msgtype="m.text", body="newer N")
        return h, older, after, newer

    def verify_receipt(self, h, receipts):
        h.receipts = receipts
        check = action_check("read_receipt", after_body="after N")
        return verify.verify_app_actions(h, dict(STATE), manifest_of(check))

    def test_read_receipt(self):
        h, older, after, newer = self.receipt_history()
        self.assertIn("read_receipt", self.verify_receipt(h, None))
        bot_only = {after["event_id"]: {"m.read": {STATE["bot_user"]: {"ts": 1}}}}
        self.assertIn("read_receipt", self.verify_receipt(h, bot_only))
        stale = {older["event_id"]: {"m.read": {APP: {"ts": 1}}}}
        self.assertIn("read_receipt", self.verify_receipt(h, stale))
        unknown = {"$nowhere": {"m.read": {APP: {"ts": 1}}}}
        self.assertIn("read_receipt", self.verify_receipt(h, unknown))
        private = {newer["event_id"]: {"m.read.private": {APP: {"ts": 1}}}}
        self.assertIn("read_receipt", self.verify_receipt(h, private))
        at = {after["event_id"]: {"m.read": {APP: {"ts": 1}}}}
        self.assertEqual(self.verify_receipt(h, at), "")
        beyond = {newer["event_id"]: {"m.read": {APP: {"ts": 1}}}}
        self.assertEqual(self.verify_receipt(h, beyond), "")

    def test_read_receipt_missing_anchor_names_the_op(self):
        h = HistoryServer([])
        h.receipts = {"$x": {"m.read": {APP: {"ts": 1}}}}
        check = action_check("read_receipt", after_body="after N")
        self.assertIn("read_receipt", verify.verify_app_actions(h, dict(STATE), manifest_of(check)))

    def verify_create_room(self, joined, room_state):
        h = HistoryServer([])
        h.joined, h.room_state = joined, room_state
        check = action_check("create_room", room_name="smoke made N", topic="smoke topic N")
        return verify.verify_app_actions(h, dict(STATE), manifest_of(check))

    def made_state(self, creator=APP, name="smoke made N", topic="smoke topic N"):
        return [
            {"type": "m.room.create", "state_key": "", "sender": creator, "content": {}},
            {"type": "m.room.name", "state_key": "", "sender": creator, "content": {"name": name}},
            {"type": "m.room.topic", "state_key": "", "sender": creator, "content": {"topic": topic}},
        ]

    def test_create_room(self):
        self.assertIn("create_room", self.verify_create_room([], {}))
        self.assertEqual(
            self.verify_create_room(["!a:localhost"], {"!a:localhost": self.made_state()}), ""
        )

    def test_create_room_needs_name_topic_and_the_app_as_creator(self):
        for state in (
            self.made_state(name="other"),
            self.made_state(topic="other"),
            self.made_state(creator=STATE["bot_user"]),
        ):
            reason = self.verify_create_room(["!a:localhost"], {"!a:localhost": state})
            self.assertIn("create_room", reason)

    def test_a_request_error_is_a_reason_not_an_exception(self):
        class Broken:
            def request(self, *a, **k):
                raise RuntimeError("boom")

        reason = verify.verify_app_actions(
            Broken(), dict(STATE), manifest_of(action_check("edit", target_body="a", new_body="b"))
        )
        self.assertIn("edit", reason)

    def test_first_failure_is_reported(self):
        good = action_check("send", text="a", match="a", msgtype="m.text")
        bad = action_check("edit", target_body="a", new_body="b")
        h = HistoryServer([])
        h.add(APP, msgtype="m.text", body="a")
        server = HistoryServer(h.events)
        reason = verify.verify_app_actions(server, dict(STATE), manifest_of(good, bad))
        self.assertIn("edit", reason)


def post_action_history(checks, drop=()):
    """The room as Synapse serves it after the app did every check correctly.

    `drop` names ops whose effect is left out (edit, delete, react, reply, pin, ...).
    """
    h = HistoryServer([])
    by_body = {}
    for c in checks:
        op = c["op"]
        if op == "send":
            ev = h.add(APP, msgtype=c["msgtype"], body=c["match"])
            if c.get("formatted_contains"):
                ev["content"]["formatted_body"] = "<span data-mx-spoiler>x</span>"
            by_body[c["text"]] = ev
        elif op in drop:
            continue
        elif op == "edit":
            t = by_body[c["target_body"]]
            h.add(
                APP,
                msgtype="m.text",
                body="* " + c["new_body"],
                **{
                    "m.new_content": {"msgtype": "m.text", "body": c["new_body"]},
                    "m.relates_to": {"rel_type": "m.replace", "event_id": t["event_id"]},
                },
            )
        elif op == "delete":
            t = by_body[c["target_body"]]
            t["content"] = {}  # Synapse strips a redacted event's content
            h.add(APP, type="m.room.redaction")["redacts"] = t["event_id"]
        elif op == "react":
            t = by_body[c["target_body"]]
            h.add(
                APP,
                type="m.reaction",
                **{
                    "m.relates_to": {
                        "rel_type": "m.annotation",
                        "event_id": t["event_id"],
                        "key": c["key"],
                    }
                },
            )
        elif op == "reply":
            t = by_body[c["target_body"]]
            by_body[c["body"]] = h.add(
                APP,
                msgtype="m.text",
                body=c["body"],
                **{"m.relates_to": {"m.in_reply_to": {"event_id": t["event_id"]}}},
            )
        elif op == "upload":
            h.add(
                APP,
                msgtype="m.image",
                body=c["filename"],
                url="mxc://localhost/up",
                info={"mimetype": c["mime"], "size": c["size"]},
            )
            h.media = {"localhost/up": b"x" * c["size"]}
        elif op == "dm":
            h.direct = {c["user"]: ["!dm:localhost"]}
            h.room_state = {
                **(h.room_state or {}),
                "!dm:localhost": [
                    {"type": "m.room.create", "state_key": "", "sender": APP, "content": {}},
                    {
                        "type": "m.room.member",
                        "state_key": APP,
                        "sender": APP,
                        "content": {"membership": "join"},
                    },
                    {
                        "type": "m.room.member",
                        "state_key": c["user"],
                        "sender": APP,
                        "content": {"membership": "invite", "is_direct": True},
                    },
                ]
            }
        elif op == "create_room":
            h.joined = ["!made:localhost"]
            h.room_state = {
                **(h.room_state or {}),
                "!made:localhost": [
                    {"type": "m.room.create", "state_key": "", "sender": APP, "content": {}},
                    {"type": "m.room.name", "state_key": "", "sender": APP, "content": {"name": c["room_name"]}},
                    {"type": "m.room.topic", "state_key": "", "sender": APP, "content": {"topic": c["topic"]}},
                ],
            }
        elif op == "read_receipt":
            anchor = by_body.get(c["after_body"])  # absent when the reply was dropped
            if anchor:
                h.receipts = {anchor["event_id"]: {"m.read": {APP: {"ts": 1}}}}
        elif op == "pin":
            t = by_body[c["target_body"]]
            h.add(APP, type="m.room.pinned_events", pinned=["$bot-pin", t["event_id"]])
        elif op == "unpin":
            h.add(APP, type="m.room.pinned_events", pinned=["$bot-pin"])
    return h


class WholeSeedTest(unittest.TestCase):
    def setUp(self):
        import tempfile

        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.checks = features.seed_app_actions(
            ROOM, "abcd", work_dir=self.tmp.name, bot_user=STATE["bot_user"]
        )

    def verify(self, drop=()):
        h = post_action_history(self.checks, drop)
        return verify.verify_app_actions(h, dict(STATE), manifest_of(*self.checks))

    def test_a_correct_run_passes(self):
        self.assertEqual(self.verify(), "")

    def test_each_dropped_effect_is_named(self):
        for op in ("edit", "delete", "react", "reply", "pin", "unpin", "upload", "dm", "read_receipt", "create_room"):
            self.assertIn(op, self.verify(drop=(op,)), op)

    def test_redacting_some_other_own_message_is_not_a_pass(self):
        # The delete target was never sent; an earlier own message is redacted.
        delete = next(c for c in self.checks if c["op"] == "delete")
        rest = [
            c
            for c in self.checks
            if c is not delete and c.get("text") != delete["target_body"]
        ]
        h = post_action_history(rest)
        other = next(e for e in h.events if e["content"].get("body") == self.checks[0]["match"])
        other["content"] = {}
        h.add(APP, type="m.room.redaction")["redacts"] = other["event_id"]
        reason = verify.verify_app_actions(h, dict(STATE), manifest_of(delete))
        self.assertIn("delete", reason)

    def test_redaction_without_a_target_field_is_not_a_pass(self):
        h = post_action_history(self.checks)
        for e in h.events:
            e.pop("redacts", None)
        self.assertIn("delete", verify.verify_app_actions(h, dict(STATE), manifest_of(*self.checks)))

    def test_the_delete_target_send_is_not_verified_by_body(self):
        delete_send = next(c for c in self.checks if c.get("text", "").endswith("delete target abcd"))
        self.assertIs(delete_send.get("verify"), False)

    def test_relations_use_the_v1_path(self):
        h = HistoryServer([])
        t = h.add(APP, msgtype="m.text", body="orig N")
        with self.assertRaises(RuntimeError):
            h.request("GET", f"/_matrix/client/v3/rooms/{ROOM}/relations/{t['event_id']}/m.replace")


class SeedAppActionsTest(unittest.TestCase):
    def checks(self):
        return features.seed_app_actions(ROOM, "abcd")

    def test_upload_writes_the_png_and_records_its_size(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            checks = features.seed_app_actions(ROOM, "abcd", work_dir=tmp)
            (up,) = [c for c in checks if c["op"] == "upload"]
            self.assertEqual(up["path"], os.path.join(tmp, "smoke-app-upload-abcd.png"))
            self.assertEqual(up["filename"], "smoke-app-upload-abcd.png")
            self.assertEqual(up["mime"], "image/png")
            with open(up["path"], "rb") as f:
                data = f.read()
            self.assertEqual(data, features._png_1x1())
            self.assertEqual(up["size"], len(data))

    def test_upload_and_dm_are_omitted_without_their_inputs(self):
        ops = [c["op"] for c in self.checks()]
        self.assertNotIn("upload", ops)
        self.assertNotIn("dm", ops)

    def test_read_receipt_is_anchored_on_the_last_message_and_comes_after_upload(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            checks = features.seed_app_actions(ROOM, "abcd", work_dir=tmp, bot_user=BOT)
        ops = [c["op"] for c in checks]
        (rr,) = [c for c in checks if c["op"] == "read_receipt"]
        self.assertEqual(rr["after_body"], "smoke app reply abcd")
        self.assertLess(ops.index("upload"), ops.index("read_receipt"))
        # /dm leaves the room, so it is the last action.
        self.assertEqual(ops[-1], "dm")

    def test_dm_names_the_bot(self):
        checks = features.seed_app_actions(ROOM, "abcd", bot_user=BOT)
        (dm,) = [c for c in checks if c["op"] == "dm"]
        self.assertEqual(dm["user"], BOT)
        self.assertEqual(dm["text"], f"/dm {BOT}")

    def test_covers_every_op_and_slash_command(self):
        checks = self.checks()
        ops = [c["op"] for c in checks]
        for op in ("send", "edit", "delete", "react", "reply", "pin", "unpin", "read_receipt"):
            self.assertIn(op, ops)
        texts = [c["text"] for c in checks if c["op"] == "send"]
        for prefix in ("/me ", "/shrug ", "/spoiler "):
            self.assertTrue([t for t in texts if t.startswith(prefix)], prefix)
        self.assertTrue([t for t in texts if not t.startswith("/")])

    def test_shape(self):
        names = set()
        for c in self.checks():
            self.assertEqual(c["kind"], "action")
            self.assertEqual(c["room_id"], ROOM)
            self.assertNotIn(c["name"], names)
            names.add(c["name"])

    def test_every_target_is_sent_by_an_earlier_check(self):
        sent = []
        for i, c in enumerate(self.checks()):
            if c["op"] == "send":
                sent.append(c["text"])
                continue
            if c["op"] in ("upload", "dm", "read_receipt", "create_room"):
                continue  # act on no earlier message
            self.assertIn(c["target_body"], sent, f"{c['name']} (#{i}) has no earlier send")

    def test_target_bodies_are_unique_and_plain(self):
        checks = self.checks()
        # unpin reverses the pin of the same message, so it shares its target.
        targets = [c["target_body"] for c in checks if "target_body" in c and c["op"] != "unpin"]
        self.assertEqual(len(targets), len(set(targets)))
        for t in targets:
            self.assertFalse(t.startswith("/"))

    def test_nonce_makes_bodies_unique_per_run(self):
        a = features.seed_app_actions(ROOM, "aaaa")
        b = features.seed_app_actions(ROOM, "bbbb")
        self.assertFalse({c["text"] for c in a if c["op"] == "send"} & {c["text"] for c in b if c["op"] == "send"})

    def test_survives_json_and_matches_verifier_expectations(self):
        import json

        checks = json.loads(json.dumps(self.checks()))
        shrug = next(c for c in checks if c["text"].startswith("/shrug "))
        self.assertTrue(shrug["match"].endswith("\u00af\\_(\u30c4)_/\u00af"))
        spoiler = next(c for c in checks if c["text"].startswith("/spoiler "))
        self.assertEqual(spoiler["formatted_contains"], "data-mx-spoiler")
        emote = next(c for c in checks if c["text"].startswith("/me "))
        self.assertEqual(emote["msgtype"], "m.emote")


if __name__ == "__main__":
    unittest.main()
