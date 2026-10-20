#!/usr/bin/env python3
"""Seed the smoke homeserver for one shell run.

Registers the app user and a `bot`, has the bot create a private unencrypted
room and a private encrypted one, invites the app user to both, has the app
user join, then has the bot post a PING in the plain room. Also records the
app user's registration device so verify.py can tell the app's own device
apart. Writes everything run.py / verify.py need to a state JSON file.
"""

import argparse
import json
import secrets
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smokelib import Server  # noqa: E402


def post_ping(server, bot_token, room):
    """Has the bot post a fresh PING in `room`; returns (ping, pong) bodies."""
    nonce = secrets.token_hex(6)
    ping = f"smoke-ping-{nonce}"
    server.request(
        "PUT",
        f"/_matrix/client/v3/rooms/{room}/send/m.room.message/{secrets.token_hex(8)}",
        token=bot_token,
        body={"msgtype": "m.text", "body": ping},
    )
    return ping, f"smoke-pong-{nonce}"


def seed(server, shared_secret, name):
    app_pw = secrets.token_urlsafe(18)
    bot_pw = secrets.token_urlsafe(18)
    app_localpart = f"smoke-{name}"
    bot_localpart = f"bot-{name}"

    app_mxid, app_token = server.register(shared_secret, app_localpart, app_pw)
    bot_mxid, bot_token = server.register(shared_secret, bot_localpart, bot_pw)

    reg_device = server.request("GET", "/_matrix/client/v3/account/whoami", token=app_token)[
        "device_id"
    ]

    room = server.request(
        "POST",
        "/_matrix/client/v3/createRoom",
        token=bot_token,
        body={
            "preset": "private_chat",
            "name": f"smoke {name}",
            "invite": [app_mxid],
        },
    )["room_id"]
    server.request("POST", f"/_matrix/client/v3/join/{room}", token=app_token, body={})

    e2ee_room = server.request(
        "POST",
        "/_matrix/client/v3/createRoom",
        token=bot_token,
        body={
            "preset": "private_chat",
            "name": f"smoke {name} encrypted",
            "invite": [app_mxid],
            "initial_state": [
                {
                    "type": "m.room.encryption",
                    "state_key": "",
                    "content": {"algorithm": "m.megolm.v1.aes-sha2"},
                }
            ],
        },
    )["room_id"]
    server.request("POST", f"/_matrix/client/v3/join/{e2ee_room}", token=app_token, body={})

    ping, pong = post_ping(server, bot_token, room)
    e2ee_nonce = secrets.token_hex(6)
    return {
        "base": server.base,
        "app_user": app_mxid,
        "app_password": app_pw,
        "bot_user": bot_mxid,
        "bot_token": bot_token,
        "room_id": room,
        "ping": ping,
        "pong": pong,
        "e2ee_room_id": e2ee_room,
        "e2ee_pong": f"smoke-e2ee-pong-{e2ee_nonce}",
        "app_token": app_token,
        "app_reg_device": reg_device,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="https://localhost:8448")
    ap.add_argument("--ca", required=True)
    ap.add_argument("--secret-file", required=True)
    ap.add_argument("--name", required=True, help="short id, e.g. qt6")
    ap.add_argument("--state", required=True, help="state JSON to write")
    args = ap.parse_args()

    server = Server(args.base, args.ca)
    server.wait_ready()
    with open(args.secret_file) as f:
        secret = f.read().strip()
    state = seed(server, secret, args.name)
    with open(args.state, "w") as f:
        json.dump(state, f)
    print(f"seeded {state['room_id']} for {state['app_user']}")


if __name__ == "__main__":
    main()
