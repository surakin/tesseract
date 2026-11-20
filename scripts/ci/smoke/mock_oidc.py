#!/usr/bin/env python3
"""Fake OpenID Connect identity provider for the SSO smoke phase (stdlib only).

  mock_oidc.py --port 8449 [--bind 127.0.0.1]   serve until killed
  mock_oidc.py --self-test                      run authorize/token/userinfo once

Endpoints (plain HTTP, no real authentication):
  GET  /authorize  client_id=smoke, redirect_uri, state[, smoke_user] -> 302 back
                   with a single-use code that maps to smoke_user (default
                   smoke-sso)
  POST /token      form grant_type=authorization_code&code=C -> access_token at-C
  GET  /userinfo   Bearer at-C -> sub / preferred_username / name = the user
  GET  /health     200, for readiness checks
Synapse is configured against it by synapse_up.py.
"""

import argparse
import json
import secrets
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CLIENT_ID = "smoke"
DEFAULT_USER = "smoke-sso"

_lock = threading.Lock()
_codes = {}  # code -> user, until redeemed at /token
_tokens = {}  # access token -> user


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write("mock_oidc: " + fmt % args + "\n")

    def _send(self, status, body=None, headers=()):
        data = b"" if body is None else json.dumps(body).encode()
        self.send_response(status)
        if body is not None:
            self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        q = {k: v[0] for k, v in urllib.parse.parse_qs(url.query).items()}
        if url.path == "/health":
            self._send(200, {"ok": True})
        elif url.path == "/authorize":
            redirect = q.get("redirect_uri")
            if q.get("client_id") != CLIENT_ID or not redirect:
                self._send(400, {"error": "invalid_request"})
                return
            code = secrets.token_urlsafe(16)
            with _lock:
                _codes[code] = q.get("smoke_user") or DEFAULT_USER
            params = {"code": code}
            if "state" in q:
                params["state"] = q["state"]
            sep = "&" if urllib.parse.urlsplit(redirect).query else "?"
            self._send(302, None, [("Location", redirect + sep + urllib.parse.urlencode(params))])
        elif url.path == "/userinfo":
            auth = self.headers.get("Authorization", "")
            with _lock:
                user = _tokens.get(auth[7:]) if auth.startswith("Bearer ") else None
            if user is None:
                self._send(401, {"error": "invalid_token"})
            else:
                self._send(200, {"sub": user, "preferred_username": user, "name": user})
        else:
            self._send(404, {"error": "not_found"})

    def do_POST(self):
        url = urllib.parse.urlsplit(self.path)
        length = int(self.headers.get("Content-Length") or 0)
        form = {k: v[0] for k, v in urllib.parse.parse_qs(self.rfile.read(length).decode()).items()}
        if url.path != "/token":
            self._send(404, {"error": "not_found"})
            return
        with _lock:
            user = _codes.pop(form.get("code", ""), None)  # single use
            if user is not None:
                _tokens["at-" + form["code"]] = user
        if form.get("grant_type") != "authorization_code" or user is None:
            self._send(400, {"error": "invalid_grant"})
        else:
            self._send(
                200,
                {"access_token": "at-" + form["code"], "token_type": "Bearer", "expires_in": 300},
            )


def make_server(bind, port):
    return ThreadingHTTPServer((bind, port), Handler)


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def self_test():
    srv = make_server("127.0.0.1", 0)
    base = f"http://127.0.0.1:{srv.server_address[1]}"
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    opener = urllib.request.build_opener(_NoRedirect)

    def call(req):
        try:
            r = opener.open(req, timeout=5)
        except urllib.error.HTTPError as e:
            r = e
        return r.status if hasattr(r, "status") else r.code, r

    q = urllib.parse.urlencode(
        {
            "response_type": "code",
            "client_id": CLIENT_ID,
            "redirect_uri": "http://127.0.0.1:1/cb",
            "state": "S1",
            "smoke_user": "smoke-sso-alice",
        }
    )
    status, r = call(urllib.request.Request(f"{base}/authorize?{q}"))
    assert status == 302, status
    loc = urllib.parse.urlsplit(r.headers["Location"])
    got = urllib.parse.parse_qs(loc.query)
    assert loc.netloc == "127.0.0.1:1" and got["state"] == ["S1"], loc
    code = got["code"][0]

    status, _ = call(urllib.request.Request(f"{base}/authorize?client_id=nope&redirect_uri=x"))
    assert status == 400, status

    def token(c):
        body = urllib.parse.urlencode({"grant_type": "authorization_code", "code": c}).encode()
        return call(urllib.request.Request(f"{base}/token", data=body))

    status, r = token(code)
    assert status == 200, status
    tok = json.loads(r.read())
    assert tok["access_token"] == "at-" + code and tok["token_type"] == "Bearer"
    status, _ = token(code)
    assert status == 400, "reused code must fail"

    req = urllib.request.Request(f"{base}/userinfo", headers={"Authorization": "Bearer " + tok["access_token"]})
    status, r = call(req)
    info = json.loads(r.read())
    assert status == 200 and info["preferred_username"] == "smoke-sso-alice", info
    req = urllib.request.Request(f"{base}/userinfo", headers={"Authorization": "Bearer bogus"})
    assert call(req)[0] == 401
    srv.shutdown()
    print("mock_oidc self-test OK")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8449)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return
    make_server(args.bind, args.port).serve_forever()


if __name__ == "__main__":
    main()
