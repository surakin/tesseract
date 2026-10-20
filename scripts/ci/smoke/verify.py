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
