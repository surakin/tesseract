#!/usr/bin/env python3
"""A minimal stdlib "browser" for the smoke test's SSO phase.

complete_sso() follows the redirect chain the app's sign-in URL starts
(Synapse -> fake provider -> Synapse -> the app's loopback listener) by hand,
keeping cookies, and stops once the loopback `/callback?loginToken=...` GET has
returned. It trusts only the smoke CA for https and allows plain http to
127.0.0.1 (the fake provider and the app's loopback listener).

  sso_browser.py --self-test      run against an in-process fake chain
"""

import http.cookiejar
import http.server
import re
import ssl
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request

MAX_HOPS = 15


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def _describe(body):
    text = body.decode(errors="replace")
    m = re.search(r"<title[^>]*>(.*?)</title>", text, re.I | re.S)
    title = m.group(1).strip() if m else ""
    rest = text[m.end() :] if m else text
    rest = re.sub(r"<(script|style)[^>]*>.*?</\1>", " ", rest, flags=re.I | re.S)
    plain = " ".join(re.sub(r"<[^>]*>", " ", rest).split())[:200]
    return f"{title}: {plain}" if title and plain else title or plain


def _is_loopback_callback(url):
    u = urllib.parse.urlsplit(url)
    return (
        u.scheme == "http"
        and u.hostname == "127.0.0.1"
        and u.path == "/callback"
        and "loginToken=" in u.query
    )


def complete_sso(auth_url, ca_file, smoke_user, timeout=60):
    """Walks the SSO redirect chain as `smoke_user`; returns the loopback URL."""
    ctx = ssl.create_default_context(cafile=ca_file)
    opener = urllib.request.build_opener(
        _NoRedirect,
        urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()),
        urllib.request.HTTPSHandler(context=ctx),
    )
    url = auth_url
    for _ in range(MAX_HOPS):
        u = urllib.parse.urlsplit(url)
        if u.scheme == "http" and u.hostname != "127.0.0.1":
            raise RuntimeError(f"SSO stuck at {url}: refusing plain http")
        if u.scheme not in ("http", "https"):
            raise RuntimeError(f"SSO stuck at {url}: unsupported scheme")
        if u.path.endswith("/authorize") and "smoke_user=" not in u.query:
            sep = "&" if u.query else "?"
            url = url + sep + urllib.parse.urlencode({"smoke_user": smoke_user})
        try:
            with opener.open(url, timeout=timeout) as r:
                status, location, body = r.status, None, r.read(2000)
        except urllib.error.HTTPError as e:
            status, location, body = e.code, e.headers.get("Location"), e.read(2000)
        except OSError:
            # The app may close the loopback connection without answering; the
            # request line, and so the token, was delivered. Anything else is real.
            if _is_loopback_callback(url):
                return url
            raise
        # The loopback GET is the end of the chain whatever it answers.
        if _is_loopback_callback(url):
            return url
        if 300 <= status < 400 and location:
            url = urllib.parse.urljoin(url, location)
            continue
        raise RuntimeError(f"SSO stuck at {url}: {status} {_describe(body)}")
    raise RuntimeError(f"SSO stuck at {url}: too many redirects")


def _self_test():
    seen = {}
    ports = {}

    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _go(self, loc):
            self.send_response(302)
            self.send_header("Location", loc)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def do_GET(self):
            u = urllib.parse.urlsplit(self.path)
            q = urllib.parse.parse_qs(u.query)
            srv = self.server.name
            if srv == "synapse" and u.path == "/redirect":
                self.send_response(302)
                self.send_header("Set-Cookie", "sess=1; Path=/")
                self.send_header("Location", f"http://127.0.0.1:{ports['idp']}/authorize")
                self.send_header("Content-Length", "0")
                self.end_headers()
            elif srv == "idp" and u.path == "/authorize":
                seen["user"] = q.get("smoke_user", [None])[0]
                self._go(f"http://127.0.0.1:{ports['synapse']}/oidc/callback")
            elif srv == "synapse" and u.path == "/oidc/callback":
                if "sess=1" not in (self.headers.get("Cookie") or ""):
                    seen["nocookie"] = True
                tok = "tok-" + str(seen.get("user"))
                self._go(f"http://127.0.0.1:{ports['loop']}/callback?loginToken={tok}")
            elif srv == "loop" and u.path == "/callback":
                seen["loop"] = q.get("loginToken", [None])[0]
                self.send_response(200)
                self.send_header("Content-Length", "2")
                self.end_headers()
                self.wfile.write(b"ok")
            elif srv == "drop" and u.path == "/callback":
                seen["drop"] = q.get("loginToken", [None])[0]
                self.close_connection = True
                self.connection.close()
            elif srv == "stuck":
                body = (
                    b"<html><head><title>Consent needed</title></head>"
                    b"<body><p>Please <b>approve</b>   the   app</p></body></html>"
                )
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            else:
                self.send_response(404)
                self.send_header("Content-Length", "0")
                self.end_headers()

    servers = []
    for name in ("synapse", "idp", "loop", "stuck", "drop"):
        s = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        s.name = name
        ports[name] = s.server_address[1]
        threading.Thread(target=s.serve_forever, daemon=True).start()
        servers.append(s)
    try:
        final = complete_sso(
            f"http://127.0.0.1:{ports['synapse']}/redirect", None, "smoke-sso-x", timeout=5
        )
        assert final.endswith("/callback?loginToken=tok-smoke-sso-x"), final
        assert seen["loop"] == "tok-smoke-sso-x", seen
        assert not seen.get("nocookie"), "cookie not replayed"
        try:
            complete_sso(f"http://127.0.0.1:{ports['stuck']}/x", None, "u", timeout=5)
        except RuntimeError as e:
            assert "Consent needed" in str(e) and "stuck at" in str(e), e
            assert "Please approve the app" in str(e), e
        else:
            raise AssertionError("stuck chain did not raise")
        drop = f"http://127.0.0.1:{ports['drop']}/callback?loginToken=tok-drop"
        assert complete_sso(drop, None, "u", timeout=5) == drop
        assert seen.get("drop") == "tok-drop", seen
    finally:
        for s in servers:
            s.shutdown()
    print("sso_browser self-test: PASS")
    return 0


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        sys.exit(_self_test())
    print(__doc__)
    sys.exit(2)
