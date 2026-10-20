"""Shared helpers for the CI smoke test (stdlib only, runs on every runner).

Talks to the throwaway Synapse over TLS, trusting only the CA that
make_ca.py generated.
"""

import hashlib
import hmac
import json
import ssl
import time
import urllib.error
import urllib.request


class Server:
    def __init__(self, base, ca_file):
        self.base = base.rstrip("/")
        self.ctx = ssl.create_default_context(cafile=ca_file)

    def request(self, method, path, token=None, body=None, timeout=30):
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(self.base + path, data=data, method=method)
        if data is not None:
            req.add_header("Content-Type", "application/json")
        if token:
            req.add_header("Authorization", "Bearer " + token)
        # Retries cover transient connection drops (the Windows runner reaches
        # Synapse in WSL2 through a netsh port proxy). Only idempotent GETs are
        # retried: a repeated POST could consume a nonce or duplicate a room.
        attempts = 3 if method == "GET" else 1
        for attempt in range(attempts):
            try:
                with urllib.request.urlopen(req, context=self.ctx, timeout=timeout) as r:
                    return json.loads(r.read() or b"{}")
            except urllib.error.HTTPError as e:
                detail = e.read().decode(errors="replace")
                raise RuntimeError(f"{method} {path} -> {e.code}: {detail}") from None
            except (urllib.error.URLError, ConnectionError, TimeoutError):
                if attempt == attempts - 1:
                    raise
                time.sleep(2)

    def wait_ready(self, timeout=90):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                self.request("GET", "/_matrix/client/versions", timeout=5)
                return
            except Exception as e:  # not up yet
                last = e
                time.sleep(1)
        raise RuntimeError(f"homeserver not ready after {timeout}s: {last}")

    def register(self, shared_secret, localpart, password, admin=False):
        """Registers through the shared-secret admin API; returns (mxid, token)."""
        nonce = self.request("GET", "/_synapse/admin/v1/register")["nonce"]
        # HMAC-SHA1 over nonce NUL user NUL password NUL admin|notadmin.
        mac = hmac.new(shared_secret.encode(), digestmod=hashlib.sha1)
        mac.update(
            b"\x00".join(
                p.encode()
                for p in (nonce, localpart, password, "admin" if admin else "notadmin")
            )
        )
        res = self.request(
            "POST",
            "/_synapse/admin/v1/register",
            body={
                "nonce": nonce,
                "username": localpart,
                "password": password,
                "admin": admin,
                "mac": mac.hexdigest(),
            },
        )
        return res["user_id"], res["access_token"]
