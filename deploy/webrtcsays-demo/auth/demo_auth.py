#!/usr/bin/env python3
"""Stage-1 login gate for the WebRTCSays demo.

Listens on port 8793 for nginx. Publish that port on 127.0.0.1 only.
Browser sessions use an HttpOnly cookie. Native clients use Authorization:
Bearer, or a query token when a header cannot be set. /demo/turn returns
the static TURN account injected on the host. This process does not mint
coturn REST credentials and does not talk to coturn.
"""

from __future__ import annotations

import hashlib
import hmac
import html
import ipaddress
import json
import os
import re
import secrets
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

COOKIE_NAME = "wsdemo"
CSRF_COOKIE = "wsdemo_csrf"
BROWSER_CLIENT = "browser"
ALLOWED_PATHS = frozenset({
    "/demo/",
    "/demo/index.html",
    "/demo/turn",
    "/demo/ws",
    "/ws",
})
ALLOWED_NEXT = frozenset({"/demo/", "/demo/index.html", "/demo/turn"})
CLIENT_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
TOKEN_RE = re.compile(r"^[A-Za-z0-9]{16,128}$")
TURN_URI_RE = re.compile(
    r"(turns?):([A-Za-z0-9.-]+)(?::(\d{1,5}))?(?:\?transport=(udp|tcp))?"
)
STATIC_TURN_NOTE = (
    "Temporary static coturn account. Short-lived TURN REST credentials "
    "are not minted. coturn use-auth-secret is not enabled, so a minted "
    "username of the form <expiry>:<name> would be rejected."
)
TURN_UNAVAILABLE_NOTE = (
    "Static TURN credentials are not available. DevOps injects them on "
    "the host. This service will not invent them and will not mint REST "
    "credentials unless coturn is changed, which is not part of stage 1."
)
TURN_MINTED_NOTE = (
    "Refusing to issue TURN credentials. Stage 1 only forwards the static "
    "account injected on the host. Minted REST credentials need a coturn "
    "use-auth-secret change that is not approved, and a minted pair would "
    "not work against the current server."
)

_WARNED_MODES: set[str] = set()
_RATE_LOCK = threading.Lock()
_RATE_HITS: dict[str, list[float]] = {}
_RATE_GLOBAL: list[float] = []


def conf_dir() -> Path:
    return Path(os.environ.get("DEMO_CONF_DIR", "/conf")).resolve()


def conf_file(name: str) -> Path:
    root = conf_dir()
    path = (root / name).resolve()
    if path.parent != root:
        raise RuntimeError("config path escaped the config directory")
    return path


def read_limited(path: Path, limit: int) -> bytes | None:
    if not path.is_file():
        return None
    warn_mode(path)
    with path.open("rb") as handle:
        data = handle.read(limit + 1)
    if len(data) > limit:
        sys.stderr.write(f"config file too large: {path.name}\n")
        return None
    return data


def strip_newline(data: bytes) -> bytes:
    if data.endswith(b"\r\n"):
        return data[:-2]
    if data.endswith(b"\n"):
        return data[:-1]
    return data


def warn_mode(path: Path) -> None:
    key = str(path)
    if key in _WARNED_MODES:
        return
    mode = path.stat().st_mode & 0o777
    if mode & 0o077:
        sys.stderr.write(f"config mode allows group or world access: {path.name}\n")
        _WARNED_MODES.add(key)


def placeholder(value: str) -> bool:
    upper = value.upper()
    return (
        value.startswith("<")
        or value.endswith(">")
        or "REPLACE" in upper
        or "EXAMPLE" in upper
        or "CHANGEME" in upper
    )


def session_key_bytes() -> bytes | None:
    try:
        raw = read_limited(conf_file("session.key"), 256)
    except RuntimeError:
        return None
    if raw is None:
        return None
    key = strip_newline(raw)
    if len(key) < 32:
        return None
    try:
        text = key.decode("ascii")
    except UnicodeDecodeError:
        text = ""
    if text and placeholder(text):
        return None
    if b"REPLACE" in key.upper() or b"EXAMPLE" in key.upper():
        return None
    return key


def load_password() -> str | None:
    try:
        raw = read_limited(conf_file("password"), 256)
    except RuntimeError:
        return None
    if raw is None:
        return None
    text = strip_newline(raw)
    if b"\n" in text or b"\r" in text:
        sys.stderr.write("password file rejected\n")
        return None
    try:
        password = text.decode("ascii")
    except UnicodeDecodeError:
        sys.stderr.write("password file rejected\n")
        return None
    if len(password) < 12 or len(password) > 128:
        sys.stderr.write("password file rejected\n")
        return None
    if any(ch.isspace() or ord(ch) < 32 or ord(ch) > 126 for ch in password):
        sys.stderr.write("password file rejected\n")
        return None
    if placeholder(password):
        sys.stderr.write("password file rejected\n")
        return None
    return password


def load_tokens() -> dict[str, str]:
    try:
        raw = read_limited(conf_file("tokens"), 65536)
    except RuntimeError:
        return {}
    if raw is None:
        return {}
    found: dict[str, str] = {}
    for line in raw.decode("utf-8", "replace").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        parts = stripped.split()
        if len(parts) != 2:
            sys.stderr.write("skipped invalid token line\n")
            continue
        client_id, secret = parts
        if not CLIENT_RE.fullmatch(client_id) or client_id == BROWSER_CLIENT:
            sys.stderr.write("skipped invalid token line\n")
            continue
        if client_id in found:
            sys.stderr.write(f"duplicate client id skipped: {client_id}\n")
            continue
        if not TOKEN_RE.fullmatch(secret) or placeholder(secret):
            sys.stderr.write(f"skipped invalid token for {client_id}\n")
            continue
        found[client_id] = secret
    return found


def ct_equal(left: bytes, right: bytes) -> bool:
    return hmac.compare_digest(
        hashlib.sha256(left).digest(),
        hashlib.sha256(right).digest(),
    )


def match_token(presented: str) -> str | None:
    items = list(load_tokens().items())
    presented_b = presented.encode("utf-8", "replace")
    found = None
    if not items:
        ct_equal(presented_b, b"dummy-token-value")
        return None
    for client_id, secret in items:
        if ct_equal(presented_b, secret.encode("utf-8")) and found is None:
            found = client_id
    return found


def match_password(presented: str) -> bool:
    password = load_password()
    presented_b = presented.encode("utf-8", "replace")
    if password is None:
        ct_equal(presented_b, b"dummy-password-value")
        return False
    return ct_equal(presented_b, password.encode("utf-8"))


def client_secret(client_id: str) -> str | None:
    if client_id == BROWSER_CLIENT:
        return load_password()
    return load_tokens().get(client_id)


def session_ttl() -> int:
    try:
        raw = int(os.environ.get("DEMO_SESSION_TTL", "43200"))
    except ValueError:
        raw = 43200
    if raw < 1:
        raw = 1
    return min(raw, 12 * 60 * 60)


def sign_session(client_id: str, secret: str, ttl: int | None = None, now: int | None = None) -> str:
    key = session_key_bytes()
    if key is None:
        raise RuntimeError("session key unavailable")
    now_i = int(time.time()) if now is None else int(now)
    lifetime = session_ttl() if ttl is None else int(ttl)
    exp = now_i + lifetime
    fingerprint = hashlib.sha256(secret.encode("utf-8")).hexdigest()
    body = f"v1.{exp}.{client_id}"
    mac = hmac.new(key, f"{body}.{fingerprint}".encode("utf-8"), hashlib.sha256).hexdigest()
    return f"{body}.{mac}"


def verify_session(token: str, now: float | None = None) -> str | None:
    now_i = time.time() if now is None else now
    parts = token.split(".")
    if len(parts) != 4 or parts[0] != "v1":
        ct_equal(b"session", b"invalid")
        return None
    _, exp_s, client_id, mac = parts
    if not exp_s.isdigit() or not CLIENT_RE.fullmatch(client_id) or not re.fullmatch(r"[0-9a-f]{64}", mac):
        ct_equal(b"session", b"invalid")
        return None
    secret = client_secret(client_id)
    key = session_key_bytes() or (b"\0" * 32)
    fingerprint = hashlib.sha256((secret or "").encode("utf-8")).hexdigest()
    body = f"v1.{exp_s}.{client_id}"
    expect = hmac.new(key, f"{body}.{fingerprint}".encode("utf-8"), hashlib.sha256).hexdigest()
    ok = hmac.compare_digest(expect, mac)
    if not ok or secret is None or key == b"\0" * 32 and session_key_bytes() is None:
        return None
    if int(exp_s) <= now_i:
        return None
    return client_id


def sign_csrf(now: int | None = None) -> str:
    key = session_key_bytes()
    if key is None:
        raise RuntimeError("session key unavailable")
    now_i = int(time.time()) if now is None else int(now)
    exp = now_i + 3600
    nonce = secrets.token_hex(16)
    body = f"v1.{exp}.{nonce}"
    mac = hmac.new(key, body.encode("utf-8"), hashlib.sha256).hexdigest()
    return f"{body}.{mac}"


def verify_csrf(value: str, now: float | None = None) -> bool:
    now_i = time.time() if now is None else now
    parts = value.split(".")
    if len(parts) != 4 or parts[0] != "v1":
        ct_equal(b"csrf", b"invalid")
        return False
    mac = parts[3]
    if not re.fullmatch(r"[0-9a-f]{64}", mac) or not re.fullmatch(r"[0-9a-f]{32}", parts[2]):
        ct_equal(b"csrf", b"invalid")
        return False
    try:
        exp = int(parts[1])
    except ValueError:
        return False
    key = session_key_bytes() or (b"\0" * 32)
    body = ".".join(parts[:3])
    expect = hmac.new(key, body.encode("utf-8"), hashlib.sha256).hexdigest()
    ok = hmac.compare_digest(expect, mac)
    if session_key_bytes() is None:
        return False
    return bool(ok and exp > now_i)


def csrf_cookie_values(header: str | None) -> list[str]:
    found = []
    for part in (header or "").split(";"):
        if "=" not in part:
            continue
        name, value = part.split("=", 1)
        if name.strip() == CSRF_COOKIE and value.strip():
            found.append(value.strip())
    return found


def csrf_pair_ok(form_value: str, cookie_value: str) -> bool:
    if not form_value or not cookie_value:
        ct_equal(b"csrf-missing", b"csrf-missing-x")
        return False
    left = form_value.encode("utf-8", "replace")
    right = cookie_value.encode("utf-8", "replace")
    if len(left) != len(right):
        ct_equal(left, b"x" * 16)
        return False
    return bool(hmac.compare_digest(left, right) and verify_csrf(form_value))


def csrf_request_ok(form_value: str, cookie_header: str | None) -> bool:
    values = csrf_cookie_values(cookie_header)
    if not values:
        ct_equal(b"csrf-missing", b"csrf-missing-x")
        return False
    matched = False
    for value in values:
        if csrf_pair_ok(form_value, value):
            matched = True
    return matched


def safe_next(raw: str | None) -> str:
    default = "/demo/"
    if not isinstance(raw, str) or raw == "" or len(raw) > 200 or raw != raw.strip():
        return default
    decoded = raw
    for _ in range(3):
        nxt = urllib.parse.unquote(decoded)
        if nxt == decoded:
            break
        decoded = nxt
    for value in (raw, decoded):
        if any(ord(ch) < 32 or ord(ch) > 126 for ch in value):
            return default
        lowered = value.lower()
        if "\\" in value or "@" in value or "://" in lowered or "//" in value:
            return default
        if "?" in value or "#" in value or "%" in value:
            return default
    if decoded in ALLOWED_NEXT:
        return decoded
    return default


def normalize_path(raw: str | None) -> str | None:
    if not isinstance(raw, str) or raw == "" or len(raw) > 512:
        return None
    path = raw.split("?", 1)[0].split("#", 1)[0]
    decoded = urllib.parse.unquote(path)
    if "%" in urllib.parse.unquote(decoded):
        return None
    if "\\" in decoded or "//" in decoded or ".." in decoded or "\x00" in decoded:
        return None
    if any(ord(ch) < 32 or ord(ch) > 126 for ch in decoded):
        return None
    if not decoded.startswith("/"):
        return None
    return decoded


def valid_turn_uri(uri: object) -> bool:
    if not isinstance(uri, str):
        return False
    match = TURN_URI_RE.fullmatch(uri)
    if not match:
        return False
    host = match.group(2)
    lowered = host.lower()
    if lowered in {"localhost", "localhost.localdomain"} or lowered.endswith((".local", ".internal", ".lan")):
        return False
    try:
        ipaddress.ip_address(host)
        return False
    except ValueError:
        pass
    labels = host.split(".")
    if len(labels) < 2 or any(label == "" or label.startswith("-") or label.endswith("-") for label in labels):
        return False
    port = match.group(3)
    if port is not None and not (1 <= int(port) <= 65535):
        return False
    return True


class TurnUnavailable(Exception):
    def __init__(self, note: str) -> None:
        self.note = note


def load_turn_public() -> dict:
    try:
        raw = read_limited(conf_file("turn.json"), 8192)
    except RuntimeError as exc:
        raise TurnUnavailable(TURN_UNAVAILABLE_NOTE) from exc
    if raw is None:
        raise TurnUnavailable(TURN_UNAVAILABLE_NOTE)
    try:
        data = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        sys.stderr.write("turn.json is not valid json\n")
        raise TurnUnavailable(TURN_UNAVAILABLE_NOTE) from exc
    if not isinstance(data, dict):
        raise TurnUnavailable(TURN_UNAVAILABLE_NOTE)
    if data.get("mode", "static") != "static" or data.get("minted") is True:
        raise TurnUnavailable(TURN_MINTED_NOTE)
    if "static-auth-secret" in data or "secret" in data or "password" in data:
        raise TurnUnavailable(TURN_MINTED_NOTE)
    username = data.get("username")
    credential = data.get("credential")
    uris = data.get("uris")
    if not isinstance(username, str) or not isinstance(credential, str) or placeholder(username) or placeholder(credential):
        raise TurnUnavailable(TURN_UNAVAILABLE_NOTE)
    if len(username) < 1 or len(credential) < 4 or len(username) > 128 or len(credential) > 256:
        raise TurnUnavailable(TURN_UNAVAILABLE_NOTE)
    if re.match(r"^[0-9]{5,}:", username):
        raise TurnUnavailable(TURN_MINTED_NOTE)
    if any(ch.isspace() or ord(ch) < 32 or ord(ch) > 126 for ch in username + credential):
        raise TurnUnavailable(TURN_UNAVAILABLE_NOTE)
    if not isinstance(uris, list) or not uris or not all(valid_turn_uri(uri) for uri in uris):
        raise TurnUnavailable(TURN_UNAVAILABLE_NOTE)
    return {
        "ok": True,
        "mode": "static",
        "temporary": True,
        "minted": False,
        "rest_api": False,
        "ttl": None,
        "username": username,
        "credential": credential,
        "uris": uris,
        "note": STATIC_TURN_NOTE,
    }


def config_errors() -> list[str]:
    errors = []
    if session_key_bytes() is None:
        errors.append("session.key missing, too short, or still a placeholder")
    if load_password() is None and not load_tokens():
        errors.append("need a password file or at least one client token")
    return errors


def login_floor_seconds() -> float:
    try:
        millis = int(os.environ.get("DEMO_LOGIN_FLOOR_MS", "40"))
    except ValueError:
        millis = 40
    if millis < 0:
        millis = 0
    return millis / 1000.0


def login_limits() -> tuple[int, float, int]:
    try:
        limit = int(os.environ.get("DEMO_LOGIN_LIMIT", "8"))
        window = float(os.environ.get("DEMO_LOGIN_WINDOW", "300"))
        global_limit = int(os.environ.get("DEMO_LOGIN_GLOBAL_LIMIT", "60"))
    except ValueError:
        return 8, 300.0, 60
    return max(1, limit), max(1.0, window), max(1, global_limit)


def rate_allow(ip: str) -> bool:
    limit, window, global_limit = login_limits()
    now = time.monotonic()
    with _RATE_LOCK:
        hits = _RATE_HITS.setdefault(ip, [])
        fresh = [stamp for stamp in hits if now - stamp < window]
        global_fresh = [stamp for stamp in _RATE_GLOBAL if now - stamp < window]
        if len(fresh) >= limit or len(global_fresh) >= global_limit:
            _RATE_HITS[ip] = fresh
            _RATE_GLOBAL[:] = global_fresh
            return False
        fresh.append(now)
        global_fresh.append(now)
        _RATE_HITS[ip] = fresh
        _RATE_GLOBAL[:] = global_fresh
        return True


def reset_rate_limits() -> None:
    with _RATE_LOCK:
        _RATE_HITS.clear()
        _RATE_GLOBAL.clear()


def is_trusted_peer(ip: str) -> bool:
    try:
        addr = ipaddress.ip_address(ip)
    except ValueError:
        return False
    return bool(addr.is_loopback or addr.is_private or addr.is_link_local)


def is_ip(value: str) -> bool:
    try:
        ipaddress.ip_address(value)
    except ValueError:
        return False
    return True


def parse_cookies(header: str | None) -> dict[str, str]:
    found: dict[str, str] = {}
    if not header:
        return found
    for part in header.split(";"):
        if "=" not in part:
            continue
        name, value = part.split("=", 1)
        name = name.strip()
        if name and name not in found:
            found[name] = value.strip()
    return found


def query_items(url_path: str) -> dict[str, str]:
    query = urllib.parse.urlsplit(url_path).query
    parsed = urllib.parse.parse_qs(query, keep_blank_values=False, max_num_fields=10, separator="&")
    return {key: values[-1] for key, values in parsed.items() if values}


def token_or_bad(value: str | None):
    if value is None or value == "":
        return None
    if value.strip() != value or any(ch.isspace() for ch in value):
        return False
    return value


def authorization_token(header: str | None):
    if header is None or header.strip() == "":
        return None
    parts = header.strip().split(" ")
    if len(parts) != 2 or parts[0].lower() != "bearer" or parts[1] == "":
        return False
    if any(ch.isspace() for ch in parts[1]):
        return False
    return parts[1]


def authenticated_client(headers, url_path: str) -> str | None:
    original = headers.get("X-Original-URI")
    if original:
        path = normalize_path(original)
    else:
        path = normalize_path(urllib.parse.urlsplit(url_path).path)
        if path == "/_demo_auth":
            path = None
    if path not in ALLOWED_PATHS:
        return None
    bearer = authorization_token(headers.get("Authorization"))
    if bearer is False:
        return None
    if isinstance(bearer, str):
        return match_token(bearer)
    demo = token_or_bad(headers.get("X-Demo-Token"))
    query = token_or_bad(query_items(url_path).get("token"))
    if demo is False or query is False:
        return None
    if isinstance(demo, str) and isinstance(query, str) and demo != query:
        hmac.compare_digest(demo.encode("utf-8"), query.encode("utf-8"))
        return None
    presented = demo or query
    if isinstance(presented, str):
        return match_token(presented)
    return verify_session(parse_cookies(headers.get("Cookie")).get(COOKIE_NAME, ""))


def origin_allowed(host_header: str | None, origin: str | None, referer: str | None) -> bool:
    if not host_header or any(ch in host_header for ch in "\r\n "):
        return False
    host = host_header.strip()
    hostname = host.split(":")[0].strip("[]").lower()
    # Browsers send Origin: null for a form POST when Referrer-Policy is
    # no-referrer. That is not a cross-site origin. A real cross-site POST
    # still fails the CSRF cookie check because the cookie is SameSite=Lax.
    if origin and origin.strip().lower() == "null":
        origin = None
    if origin:
        parsed = urllib.parse.urlsplit(origin.strip())
        if parsed.scheme not in {"https", "http"} or parsed.netloc == "":
            return False
        if parsed.scheme != "https" and hostname not in {"localhost", "127.0.0.1", "::1"}:
            return False
        return parsed.netloc.lower() == host.lower()
    if referer:
        parsed = urllib.parse.urlsplit(referer.strip())
        if parsed.netloc.lower() != host.lower():
            return False
    return True


def session_cookie(value: str, max_age: int) -> str:
    return (
        f"{COOKIE_NAME}={value}; HttpOnly; Secure; SameSite=Lax; Path=/; Max-Age={max_age}"
    )


def clear_session_cookie() -> str:
    return f"{COOKIE_NAME}=; HttpOnly; Secure; SameSite=Lax; Path=/; Max-Age=0"


def csrf_cookie(value: str) -> str:
    return (
        f"{CSRF_COOKIE}={value}; HttpOnly; Secure; SameSite=Lax; Path=/demo; Max-Age=3600"
    )


def login_page(csrf: str, nxt: str, message: str) -> bytes:
    banner = ""
    if message:
        banner = f'<p class="msg err" role="status">{html.escape(message, quote=True)}</p>'
    body = f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="color-scheme" content="dark">
  <meta name="referrer" content="no-referrer">
  <meta name="robots" content="noindex, nofollow">
  <title>WebRTCSays demo login</title>
  <style>
    body.wsdemo {{ margin: 0; background: #1B2838; color: #C7D5E0; font: 14px/1.4 Inter, system-ui, sans-serif; }}
    body.wsdemo * {{ box-sizing: border-box; }}
    body.wsdemo .wrap {{ max-width: 1120px; margin: 0 auto; padding: 0 32px; }}
    body.wsdemo .narrow {{ max-width: 520px; margin: 40px auto 48px; }}
    body.wsdemo .top {{ background: #171D25; border-bottom: 3px solid #2A475E; }}
    body.wsdemo .topin {{ display: flex; align-items: center; min-height: 72px; }}
    body.wsdemo .wm {{ margin: 0; font-size: 22px; font-weight: 800; letter-spacing: -.01em; line-height: 1.2; color: #FFFFFF; }}
    body.wsdemo .by {{ margin: 0; font-size: 12px; color: #8F98A0; text-transform: uppercase; letter-spacing: .12em; }}
    body.wsdemo .card {{ background: #2A475E; border: 1px solid #3D4450; border-radius: 10px; padding: 24px; }}
    body.wsdemo h2 {{ margin: 0 0 16px; font-size: 13px; line-height: 1.3; font-weight: 700; text-transform: uppercase; letter-spacing: .1em; color: #D6D9DC; }}
    body.wsdemo p {{ margin: 0 0 16px; }}
    body.wsdemo label {{ display: block; font-weight: 600; margin: 0 0 8px; color: #F1F2F3; }}
    body.wsdemo input:not([type=hidden]) {{ min-height: 48px; padding: 0 14px; font: inherit; width: 100%; background: #0E141B; border: 1px solid #ACB2B8; border-radius: 6px; color: #C7D5E0; }}
    body.wsdemo .btn {{ min-height: 48px; padding: 0 20px; font: inherit; font-size: 15px; font-weight: 600; cursor: pointer; border-radius: 6px; border: 1px solid #1A9FFF; color: #0E141B; background: #1A9FFF; touch-action: manipulation; }}
    body.wsdemo .full {{ width: 100%; margin-top: 20px; }}
    body.wsdemo .msg.err {{ padding: 14px 18px; border-radius: 8px; background: #1B2838; border: 1px solid #E35E1C; color: #F1F2F3; }}
    body.wsdemo :focus-visible {{ outline: 3px solid #66C0F4; outline-offset: 2px; }}
    @media (max-width: 800px) {{
      body.wsdemo .wrap {{ padding: 0 16px; }}
      body.wsdemo .card {{ padding: 16px; }}
      body.wsdemo .topin {{ min-height: 0; padding: 14px 0; }}
    }}
  </style>
</head>
<body class="wsdemo">
  <header class="top">
    <div class="wrap topin">
      <div class="brand">
        <h1 class="wm">WebRTCSays</h1>
        <p class="by">by Wilddolphin LLC</p>
      </div>
    </div>
  </header>
  <main>
    <section class="narrow">
      <div class="card">
        <h2>Sign in</h2>
        <p>Room room101, callee Slim. The password or client token is exchanged for a cookie and is not put in the address bar.</p>
        {banner}
        <form method="post" action="/demo/login" autocomplete="on">
          <input type="hidden" name="csrf" value="{html.escape(csrf, quote=True)}">
          <input type="hidden" name="next" value="{html.escape(nxt, quote=True)}">
          <label for="credential">Password or client token</label>
          <input id="credential" name="credential" type="password" autocomplete="current-password" minlength="12" maxlength="128" required>
          <button class="btn full" type="submit">Sign in</button>
        </form>
      </div>
    </section>
  </main>
</body>
</html>
"""
    return body.encode("utf-8")


class DemoHandler(BaseHTTPRequestHandler):
    server_version = "webrtcsays-demo"
    sys_version = ""
    protocol_version = "HTTP/1.1"

    def version_string(self) -> str:
        return self.server_version

    def log_message(self, fmt: str, *args) -> None:
        return

    def log_request(self, code: str | int = "-", size: str | int = "-") -> None:
        path = urllib.parse.urlsplit(self.path).path
        sys.stderr.write(f"{self.command} {path} {code}\n")

    def setup(self) -> None:
        super().setup()
        self.connection.settimeout(10)

    def do_GET(self) -> None:
        self._dispatch()

    def do_HEAD(self) -> None:
        self._dispatch()

    def do_POST(self) -> None:
        self._dispatch()

    def _dispatch(self) -> None:
        path = urllib.parse.urlsplit(self.path).path
        try:
            if path == "/healthz" and self.command in {"GET", "HEAD"}:
                self._json(200, {"ok": True})
                return
            if path == "/_demo_auth" and self.command in {"GET", "HEAD"}:
                self._auth_check()
                return
            if path == "/demo/login" and self.command in {"GET", "HEAD"}:
                self._login_get()
                return
            if path == "/demo/login" and self.command == "POST":
                self._login_post()
                return
            if path == "/demo/logout" and self.command == "POST":
                self._logout()
                return
            if path == "/demo/turn" and self.command in {"GET", "HEAD"}:
                self._turn()
                return
            if self.command not in {"GET", "HEAD", "POST"}:
                self._json(405, {"ok": False, "error": "method"}, extra=[("Allow", "GET, HEAD, POST")])
                return
            self._json(404, {"ok": False, "error": "not_found"})
        except Exception as exc:
            sys.stderr.write(f"{self.command} {path} 500 {type(exc).__name__}\n")
            if not getattr(self, "_sent", False):
                self._json(500, {"ok": False, "error": "internal"})

    def _auth_check(self) -> None:
        if session_key_bytes() is None:
            sys.stderr.write("session key unavailable\n")
            self._json(401, {"ok": False, "error": "unauthorized"})
            return
        client = authenticated_client(self.headers, self.path)
        if not client:
            self._json(401, {"ok": False, "error": "unauthorized"}, extra=[
                ("WWW-Authenticate", 'Bearer realm="webrtcsays-demo"'),
            ])
            return
        self._respond(204, b"", "text/plain; charset=utf-8")

    def _login_get(self) -> None:
        if session_key_bytes() is None:
            sys.stderr.write("session key unavailable\n")
            self._json(503, {"ok": False, "error": "unavailable"})
            return
        csrf = sign_csrf()
        nxt = safe_next(query_items(self.path).get("next"))
        page = login_page(csrf, nxt, "")
        self._respond(
            200,
            page,
            "text/html; charset=utf-8",
            cookies=[csrf_cookie(csrf)],
            extra=[("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'")],
        )

    def _login_post(self) -> None:
        started = time.monotonic()
        try:
            if session_key_bytes() is None or (load_password() is None and not load_tokens()):
                sys.stderr.write("login unavailable\n")
                self._json(503, {"ok": False, "error": "unavailable"})
                return
            form = self._form()
            if form is None:
                return
            host = self.headers.get("Host")
            if not origin_allowed(host, self.headers.get("Origin"), self.headers.get("Referer")):
                self._reject_login(403, "Login was rejected.")
                return
            ip = self._client_ip()
            if not rate_allow(ip):
                ct_equal(b"rate-limit", b"rate-limit-x")
                self._respond(
                    429,
                    b"Try again later\n",
                    "text/plain; charset=utf-8",
                    extra=[("Retry-After", "60")],
                )
                return
            if not csrf_request_ok(form.get("csrf", ""), self.headers.get("Cookie")):
                self._reject_login(403, "Login was rejected.")
                return
            credential = form.get("credential", "")
            if len(credential) > 128:
                credential = ""
            token_id = match_token(credential)
            password_ok = match_password(credential)
            if token_id:
                client_id = token_id
                secret = load_tokens().get(token_id)
            elif password_ok:
                client_id = BROWSER_CLIENT
                secret = load_password()
            else:
                client_id = None
                secret = None
            if not client_id or not secret:
                self._reject_login(401, "Those credentials were not accepted.")
                return
            value = sign_session(client_id, secret)
            nxt = safe_next(form.get("next"))
            self._respond(
                303,
                b"Signed in\n",
                "text/plain; charset=utf-8",
                cookies=[session_cookie(value, session_ttl())],
                extra=[("Location", nxt)],
            )
        finally:
            remain = login_floor_seconds() - (time.monotonic() - started)
            if remain > 0:
                time.sleep(remain)

    def _reject_login(self, status: int, message: str) -> None:
        csrf = sign_csrf()
        nxt = "/demo/"
        self._respond(
            status,
            login_page(csrf, nxt, message),
            "text/html; charset=utf-8",
            cookies=[csrf_cookie(csrf)],
        )

    def _logout(self) -> None:
        self._respond(
            303,
            b"Signed out\n",
            "text/plain; charset=utf-8",
            cookies=[clear_session_cookie()],
            extra=[("Location", "/demo/login")],
        )

    def _turn(self) -> None:
        client = authenticated_client(self.headers, self.path)
        if not client:
            self._json(401, {"ok": False, "error": "unauthorized"})
            return
        try:
            payload = load_turn_public()
        except TurnUnavailable as exc:
            sys.stderr.write("turn credentials withheld\n")
            self._json(503, {
                "ok": False,
                "error": "turn_unavailable",
                "mode": "static",
                "temporary": True,
                "minted": False,
                "rest_api": False,
                "note": exc.note,
            })
            return
        self._json(200, payload)

    def _form(self) -> dict[str, str] | None:
        ctype = self.headers.get("Content-Type", "")
        if not ctype.startswith("application/x-www-form-urlencoded"):
            self._json(415, {"ok": False, "error": "unsupported_type"})
            return None
        raw_len = self.headers.get("Content-Length", "")
        if not raw_len.isdigit():
            self._json(400, {"ok": False, "error": "bad_request"})
            return None
        length = int(raw_len)
        if length > 4096:
            self._json(413, {"ok": False, "error": "too_large"})
            return None
        raw = self.rfile.read(length) if length else b""
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            self._json(400, {"ok": False, "error": "bad_request"})
            return None
        parsed = urllib.parse.parse_qs(text, keep_blank_values=True, max_num_fields=8, separator="&")
        return {key: values[-1] for key, values in parsed.items() if values}

    def _client_ip(self) -> str:
        peer = self.client_address[0]
        if os.environ.get("DEMO_TRUST_PROXY", "0") == "1" and is_trusted_peer(peer):
            forwarded = self.headers.get("X-Real-IP", "").strip()
            if forwarded and "," not in forwarded and is_ip(forwarded):
                return forwarded
        return peer

    def _json(self, status: int, payload: dict, extra: list[tuple[str, str]] | None = None) -> None:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self._respond(status, body, "application/json; charset=utf-8", extra=extra)

    def _respond(
        self,
        status: int,
        body: bytes,
        content_type: str,
        cookies: list[str] | None = None,
        extra: list[tuple[str, str]] | None = None,
    ) -> None:
        if getattr(self, "_sent", False):
            return
        self._sent = True
        data = body if self.command != "HEAD" else b""
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store, private")
        self.send_header("Pragma", "no-cache")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        for name, value in extra or []:
            if any(ch in name + value for ch in "\r\n"):
                continue
            self.send_header(name, value)
        for cookie in cookies or []:
            if any(ch in cookie for ch in "\r\n"):
                continue
            self.send_header("Set-Cookie", cookie)
        self.end_headers()
        if data:
            self.wfile.write(data)


class DemoServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address: tuple[str, int]) -> None:
        super().__init__(address, DemoHandler)


def serve(bind: str, port: int) -> DemoServer:
    server = DemoServer((bind, port))
    thread = threading.Thread(target=server.serve_forever, name="demo-auth", daemon=True)
    thread.start()
    return server


def main() -> int:
    errors = config_errors()
    if errors:
        for item in errors:
            sys.stderr.write(f"config error: {item}\n")
        return 2
    bind = os.environ.get("DEMO_BIND", "127.0.0.1")
    port = int(os.environ.get("DEMO_PORT", "8793"))
    server = DemoServer((bind, port))
    sys.stderr.write(f"listening {bind}:{port}\n")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        return 0
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
