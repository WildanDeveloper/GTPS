#!/usr/bin/env python3
"""WildanDev GTPS login service (English only).

Serves over HTTPS on 127.0.0.1:8092 using the certificate in ../resources/certs:
  * POST /growtopia/server_data.php -> contents of ../resources/server_data.php
  * GET/POST /player/login/dashboard -> login page carrying a base64 _token
  * POST /player/growid/login/validate -> JSON with the account token

The game server later decodes the token (base64 "_token=..&growId=..&password=..").
"""
import base64
import json
import os
import re
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote_plus
import ssl

HOST, PORT = "127.0.0.1", 8092
BASE = os.path.dirname(os.path.abspath(__file__))
RESOURCES = os.path.join(BASE, "..", "resources")

MAX_BODY_BYTES = 64 * 1024
GROWID_RE = re.compile(r"^[A-Za-z0-9_]{1,32}$")
# Printable ASCII without '&' (ambiguous inside the token format).
PASSWORD_RE = re.compile(r"^[ -~]{1,64}$")

PAGE = """<!doctype html><html><head><meta charset="utf-8">
<title>WildanDev GTPS login</title></head><body>
<h2>WildanDev GTPS</h2>
<form method="POST" action="/player/growid/login/validate">
<input type="hidden" name="_token" value="__TOKEN__">
<label>GrowID: <input name="growId" required></label><br>
<label>Password: <input type="password" name="password" required></label><br>
<button type="submit">Login / Register</button>
</form>
</body></html>"""


def encode(text):
    return base64.b64encode(text.encode()).decode()


def decode(text):
    try:
        padded = text + "=" * (-len(text) % 4)
        return base64.b64decode(padded).decode("utf-8", "ignore")
    except Exception:
        return ""


def pick_field(query, *names):
    for name in names:
        values = query.get(name)
        if values and values[0].strip():
            return values[0].strip()
    return ""


def load_server_data():
    with open(os.path.join(RESOURCES, "server_data.php"), "rb") as handle:
        return handle.read()


# 1x1 transparent PNG, so login webviews never see a favicon 404.
FAVICON_PNG = bytes([
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
    0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
    0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49,
    0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
])

# Tokens this process issued. A refresh (checktoken) must present a token we
# actually issued; anything else is rejected instead of blindly re-encoding
# whatever blob the client posted.
_ISSUED_LOCK = threading.Lock()
_ISSUED_TOKENS = {}


def remember_token(token, grow_id, password):
    with _ISSUED_LOCK:
        _ISSUED_TOKENS[token] = (grow_id, password)


def known_token(token):
    with _ISSUED_LOCK:
        return _ISSUED_TOKENS.get(token)


class Handler(BaseHTTPRequestHandler):
    server_version = "WildanDevLogin/1.1"
    timeout = 30  # Slow-loris guard: stalled clients are dropped.

    def _body(self):
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            return ""
        if length <= 0:
            return ""
        # Never read more than the cap; oversize bodies are truncated so one
        # client cannot balloon server memory.
        return self.rfile.read(min(length, MAX_BODY_BYTES)).decode("utf-8", "ignore")

    def _send(self, code, data, content_type):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _serve_server_data(self):
        try:
            content = load_server_data()
        except OSError:
            return self._send(500, b"server_data unavailable", "text/plain")
        # Single header set: BaseHTTPRequestHandler adds the status line and
        # this Content-Type; the payload must never embed its own headers.
        return self._send(200, content, "text/plain")

    def do_GET(self):
        if self.path == "/":
            return self._send(200, b"WildanDev login ok", "text/plain")
        if self.path == "/favicon.ico":
            return self._send(200, FAVICON_PNG, "image/png")
        if self.path.startswith("/player/validate/close"):
            return self._send(200, b'{"status":"close"}', "application/json")
        if self.path.endswith("/growtopia/server_data.php"):
            return self._serve_server_data()
        if self.path.startswith("/player/login/dashboard"):
            return self._send(200, PAGE.replace("__TOKEN__", encode("proto=225")).encode(), "text/html")
        return self._send(404, b"not found", "text/plain")

    def do_POST(self):
        raw = self._body()
        if self.path.endswith("/growtopia/server_data.php"):
            return self._serve_server_data()
        if self.path.startswith("/player/growid/checktoken"):
            # Preserve method and body across the redirect.
            self.send_response(307)
            self.send_header("Location", "/player/growid/validate/checktoken")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path.startswith("/player/growid/validate/checktoken"):
            return self._handle_checktoken(raw)
        if self.path.startswith("/player/login/dashboard"):
            first = unquote_plus(raw.split("&")[0]) if raw else "proto=225"
            if "=" in first and not first.startswith("proto"):
                first = first.split("=", 1)[0]
            return self._send(200, PAGE.replace("__TOKEN__", encode(first)).encode(), "text/html")
        if self.path.startswith("/player/growid/login/validate"):
            return self._handle_validate(raw)
        return self._send(404, b"not found", "text/plain")

    def _handle_validate(self, raw):
        query = parse_qs(raw, keep_blank_values=True)
        grow = (query.get("growId", [""])[0] or "").strip()
        password = (query.get("password", [""])[0] or "").strip()
        token = (query.get("_token", [""])[0] or "").strip()
        if not grow or not password:
            return self._send(400, json.dumps(
                {"status": "error", "message": "growId/password required"}).encode(),
                "application/json")
        # Same charset the game server enforces, so a web-accepted account
        # can never fail (or inject) at the game layer.
        if not GROWID_RE.match(grow):
            return self._send(400, json.dumps(
                {"status": "error",
                 "message": "GrowID: 1-32 letters, digits or underscore"}).encode(),
                "application/json")
        if not PASSWORD_RE.match(password):
            return self._send(400, json.dumps(
                {"status": "error",
                 "message": "Password: 1-64 printable characters, no '&'"}).encode(),
                "application/json")
        account = encode("_token=%s&growId=%s&password=%s" % (token, grow, password))
        remember_token(account, grow, password)
        body = json.dumps(
            {"status": "success", "message": "Account Validated.",
             "token": account, "url": "", "accountType": "growtopia"}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        # growId is validated above, so it cannot inject headers. Cookies are
        # HttpOnly (no script access) and Secure (HTTPS-only service).
        self.send_header("Set-Cookie", "gtps_token=%s; Path=/; HttpOnly; Secure" % account)
        self.send_header("Set-Cookie", "growId=%s; Path=/; HttpOnly; Secure" % grow)
        self.end_headers()
        self.wfile.write(body)

    def _handle_checktoken(self, raw):
        # Only tokens issued by this process may be refreshed, and the
        # refreshed credentials come from the registry, not from the blob the
        # client posted.
        query = parse_qs(raw, keep_blank_values=True)
        refresh = pick_field(query, "refreshToken", "refresh_token", "token")
        client_data = pick_field(query, "clientData", "client_data", "_token")
        known = known_token(refresh) if refresh else None
        if known is None:
            return self._send(401, json.dumps(
                {"status": "error", "message": "invalid refresh token"}).encode(),
                "application/json")
        grow, password = known
        account = encode("_token=%s&growId=%s&password=%s" % (client_data, grow, password))
        remember_token(account, grow, password)
        return self._send(200, json.dumps(
            {"status": "success", "message": "Token is valid.",
             "token": account, "url": "", "accountType": "growtopia"}).encode(),
            "application/json")

    def do_PUT(self):
        return self.do_POST()


if __name__ == "__main__":
    # ThreadingHTTPServer: one stalled login no longer blocks every other
    # client (the old single-threaded HTTPServer did).
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    server.daemon_threads = True
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(os.path.join(RESOURCES, "certs", "server.crt"),
                            os.path.join(RESOURCES, "certs", "server.key"))
    server.socket = context.wrap_socket(server.socket, server_side=True)
    print("WildanDev login service on https://%s:%d" % (HOST, PORT))
    server.serve_forever()
