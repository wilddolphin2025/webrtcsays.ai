#!/usr/bin/env python3
"""HTTP tests for the stage-1 demo auth service."""

from __future__ import annotations

import hmac
import json
import os
import sys
import threading
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "auth"))

import demo_auth  # noqa: E402


PASSWORD = "browserpass1234"
TOKEN_ID = "founder-iphone"
TOKEN = "a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6"
OTHER = "b1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6"


class AuthTests(unittest.TestCase):
    def setUp(self) -> None:
        self._env = os.environ.copy()
        self.conf = Path(self._mkdtemp())
        os.environ["DEMO_CONF_DIR"] = str(self.conf)
        os.environ["DEMO_TRUST_PROXY"] = "1"
        os.environ["DEMO_LOGIN_FLOOR_MS"] = "0"
        os.environ["DEMO_LOGIN_LIMIT"] = "40"
        os.environ["DEMO_LOGIN_WINDOW"] = "300"
        os.environ["DEMO_LOGIN_GLOBAL_LIMIT"] = "100"
        os.environ["DEMO_SESSION_TTL"] = "43200"
        (self.conf / "session.key").write_text("k" * 48, encoding="utf-8")
        (self.conf / "password").write_text(PASSWORD + "\n", encoding="utf-8")
        (self.conf / "tokens").write_text(
            f"# comment\n{TOKEN_ID} {TOKEN}\nvr-babelfish {OTHER}\n",
            encoding="utf-8",
        )
        os.chmod(self.conf / "session.key", 0o600)
        os.chmod(self.conf / "password", 0o600)
        os.chmod(self.conf / "tokens", 0o600)
        (self.conf / "turn.json").write_text(json.dumps({
            "mode": "static",
            "username": "stage1user",
            "credential": "stage1pass",
            "uris": [
                "turn:turn.wilddolphin.us:3478?transport=udp",
                "turn:turn.wilddolphin.us:3478?transport=tcp",
                "turns:turn.wilddolphin.us:5349?transport=tcp",
            ],
        }), encoding="utf-8")
        os.chmod(self.conf / "turn.json", 0o600)
        demo_auth.reset_rate_limits()
        demo_auth._WARNED_MODES.clear()
        self.server = demo_auth.DemoServer(("127.0.0.1", 0))
        self.thread = threading.Thread(
            target=self.server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True
        )
        self.thread.start()
        self.base = f"http://127.0.0.1:{self.server.server_address[1]}"
        self.logs = []
        self._stderr = sys.stderr

        class NoRedirect(urllib.request.HTTPRedirectHandler):
            def http_error_302(self, req, fp, code, msg, headers):
                raise urllib.error.HTTPError(req.full_url, code, msg, headers, fp)

            http_error_301 = http_error_303 = http_error_307 = http_error_308 = http_error_302

        self.opener = urllib.request.build_opener(NoRedirect)

    def tearDown(self) -> None:
        sys.stderr = self._stderr
        self.server.shutdown()
        self.server.server_close()
        os.environ.clear()
        os.environ.update(self._env)
        self._tmp.cleanup()

    def _mkdtemp(self) -> str:
        import tempfile
        self._tmp = tempfile.TemporaryDirectory()
        return self._tmp.name

    def request(self, method, path, body=None, headers=None, capture=False):
        data = body.encode() if isinstance(body, str) else body
        req = urllib.request.Request(self.base + path, data=data, method=method, headers=headers or {})
        buf = None
        if capture:
            import io
            buf = io.StringIO()
            sys.stderr = buf
        try:
            try:
                with self.opener.open(req, timeout=3) as resp:
                    payload = resp.read()
                    info = resp.headers
                    code = resp.status
            except urllib.error.HTTPError as exc:
                payload = exc.read()
                info = exc.headers
                code = exc.code
        finally:
            if capture:
                sys.stderr = self._stderr
                self.logs.append(buf.getvalue())
        return code, payload, info

    def csrf(self):
        code, body, headers = self.request("GET", "/demo/login")
        self.assertEqual(code, 200)
        text = body.decode()
        marker = 'name="csrf" value="'
        csrf = text.split(marker, 1)[1].split('"', 1)[0]
        cookie = ""
        for item in headers.get_all("Set-Cookie") or []:
            if item.startswith("wsdemo_csrf="):
                cookie = item.split(";", 1)[0]
        self.assertTrue(csrf)
        self.assertIn(csrf, cookie)
        return csrf, cookie

    def login(self, credential, nxt="/demo/", origin=None, csrf=None, cookie=None, extra_headers=None):
        if csrf is None or cookie is None:
            csrf, cookie = self.csrf()
        form = urllib.parse.urlencode({
            "credential": credential,
            "csrf": csrf,
            "next": nxt,
        })
        headers = {
            "Content-Type": "application/x-www-form-urlencoded",
            "Cookie": cookie,
        }
        if origin is not None:
            headers["Origin"] = origin
        if extra_headers:
            headers.update(extra_headers)
        return self.request("POST", "/demo/login", form, headers, capture=True)

    def session_cookie(self, headers) -> str | None:
        for item in headers.get_all("Set-Cookie") or []:
            if item.startswith("wsdemo="):
                return item
        return None

    def test_bad_login_and_good_login_cookie(self) -> None:
        code, body, headers = self.login("not-the-password")
        self.assertEqual(code, 401)
        self.assertIsNone(self.session_cookie(headers))
        self.assertNotIn(PASSWORD.encode(), body)
        self.assertIn(b"not accepted", body)
        joined = "\n".join(self.logs)
        self.assertNotIn("not-the-password", joined)
        self.assertNotIn(PASSWORD, joined)

        code, body, headers = self.login(PASSWORD)
        self.assertEqual(code, 303, body)
        cookie = self.session_cookie(headers)
        self.assertIsNotNone(cookie)
        self.assertIn("HttpOnly", cookie)
        self.assertIn("Secure", cookie)
        self.assertIn("SameSite=Lax", cookie)
        self.assertIn("Path=/", cookie)
        self.assertIn("Max-Age=43200", cookie)
        self.assertEqual(headers.get("Location"), "/demo/")
        self.assertNotIn(PASSWORD, headers.get("Location"))
        self.assertNotIn(PASSWORD.encode(), body)
        name, value = cookie.split(";", 1)[0].split("=", 1)
        self.assertEqual(name, "wsdemo")
        code, _, _ = self.request(
            "GET",
            "/_demo_auth",
            headers={"Cookie": f"wsdemo={value}", "X-Original-URI": "/demo/ws"},
        )
        self.assertEqual(code, 204)

    def test_token_login_uses_client_id(self) -> None:
        code, _, headers = self.login(TOKEN)
        self.assertEqual(code, 303)
        value = self.session_cookie(headers).split(";", 1)[0].split("=", 1)[1]
        client = demo_auth.verify_session(value)
        self.assertEqual(client, TOKEN_ID)

    def test_password_is_not_a_bearer_token(self) -> None:
        code, body, _ = self.request(
            "GET",
            "/_demo_auth",
            headers={"Authorization": f"Bearer {PASSWORD}", "X-Original-URI": "/demo/ws"},
        )
        self.assertEqual(code, 401)
        self.assertNotIn(PASSWORD.encode(), body)

    def test_bearer_and_query_and_preference(self) -> None:
        code, _, _ = self.request(
            "GET",
            "/_demo_auth",
            headers={"Authorization": f"Bearer {TOKEN}", "X-Original-URI": "/demo/ws"},
        )
        self.assertEqual(code, 204)
        code, _, _ = self.request(
            "GET",
            f"/_demo_auth?token={TOKEN}",
            headers={"X-Original-URI": "/ws"},
            capture=True,
        )
        self.assertEqual(code, 204)
        self.assertNotIn(TOKEN, "\n".join(self.logs))
        code, _, _ = self.request(
            "GET",
            f"/_demo_auth?token={TOKEN}",
            headers={"Authorization": "Bearer not-a-valid-token", "X-Original-URI": "/demo/ws"},
        )
        self.assertEqual(code, 401)
        code, _, _ = self.request(
            "GET",
            "/demo/turn",
            headers={"X-Demo-Token": TOKEN, "X-Original-URI": "/demo/turn"},
        )
        self.assertEqual(code, 200)

    def test_expiry_and_tamper_and_revoke(self) -> None:
        expired = demo_auth.sign_session(TOKEN_ID, TOKEN, ttl=-30)
        code, _, _ = self.request(
            "GET",
            "/_demo_auth",
            headers={"Cookie": f"wsdemo={expired}", "X-Original-URI": "/demo/"},
        )
        self.assertEqual(code, 401)
        fresh = demo_auth.sign_session(TOKEN_ID, TOKEN, ttl=60)
        flipped = fresh[:-1] + ("a" if fresh[-1] != "a" else "b")
        code, _, _ = self.request(
            "GET",
            "/_demo_auth",
            headers={"Cookie": f"wsdemo={flipped}", "X-Original-URI": "/demo/"},
        )
        self.assertEqual(code, 401)
        code, _, headers = self.login(TOKEN)
        value = self.session_cookie(headers).split(";", 1)[0].split("=", 1)[1]
        exp = int(value.split(".")[1])
        self.assertAlmostEqual(exp, int(time.time()) + 43200, delta=5)
        (self.conf / "tokens").write_text(f"vr-babelfish {OTHER}\n", encoding="utf-8")
        code, _, _ = self.request(
            "GET",
            "/_demo_auth",
            headers={"Cookie": f"wsdemo={value}", "X-Original-URI": "/demo/ws"},
        )
        self.assertEqual(code, 401)

    def test_path_auth(self) -> None:
        headers = {"Authorization": f"Bearer {TOKEN}"}
        allowed = ["/demo/", "/demo/index.html", "/demo/turn", "/demo/ws", "/ws"]
        for path in allowed:
            code, _, _ = self.request("GET", "/_demo_auth", headers={**headers, "X-Original-URI": path})
            self.assertEqual(code, 204, path)
        denied = ["/", "/babelfish/healthz", "/demo/login", "/ws/", "/demo/ws/extra", "/demo/../ws", ""]
        for path in denied:
            code, _, _ = self.request("GET", "/_demo_auth", headers={**headers, "X-Original-URI": path})
            self.assertEqual(code, 401, path)
        code, _, _ = self.request("GET", "/_demo_auth", headers=headers)
        self.assertEqual(code, 401)

    def test_redirects_are_closed(self) -> None:
        hostile = [
            "https://evil.example/phish",
            "//evil.example",
            "/\\evil.example",
            "/demo/login",
            "/demo/../../etc/passwd",
            "/demo/%2e%2e/secret",
            "/demo/%2f%2fevil.example",
            "/demo/ws?token=sekret",
            "http://rtc.wilddolphin.us/demo/",
            "/ws",
            "/demo/\r\nLocation: https://evil.example",
        ]
        for nxt in hostile:
            code, body, headers = self.login(PASSWORD, nxt=nxt)
            self.assertEqual(code, 303, nxt)
            self.assertEqual(headers.get("Location"), "/demo/", nxt)
            self.assertNotIn(b"evil.example", body)
            self.assertNotIn("evil.example", headers.get("Location"))
        code, body, _ = self.request("GET", "/demo/login?next=https://evil.example/phish")
        self.assertEqual(code, 200)
        self.assertNotIn(b"evil.example", body)
        self.assertIn(b'name="next" value="/demo/"', body)
        code, _, headers = self.login(PASSWORD, nxt="/demo/index.html")
        self.assertEqual(headers.get("Location"), "/demo/index.html")

    def test_csrf_and_origin(self) -> None:
        csrf, cookie = self.csrf()
        form = urllib.parse.urlencode({"credential": PASSWORD, "csrf": "deadbeef", "next": "/demo/"})
        code, _, headers = self.request(
            "POST",
            "/demo/login",
            form,
            {"Content-Type": "application/x-www-form-urlencoded", "Cookie": cookie},
        )
        self.assertEqual(code, 403)
        self.assertIsNone(self.session_cookie(headers))
        code, _, headers = self.login(PASSWORD, origin="https://evil.example")
        self.assertEqual(code, 403)
        self.assertIsNone(self.session_cookie(headers))
        code, _, headers = self.login(PASSWORD, origin="null")
        self.assertEqual(code, 303, "Origin null is what Chrome sends under no-referrer")
        self.assertIsNotNone(self.session_cookie(headers))
        code, _, headers = self.login(PASSWORD, origin=self.base)
        self.assertEqual(code, 303)
        self.assertIsNotNone(self.session_cookie(headers))

    def test_login_rate_limit(self) -> None:
        os.environ["DEMO_LOGIN_LIMIT"] = "3"
        demo_auth.reset_rate_limits()
        codes = []
        for _ in range(4):
            code, _, _ = self.login("wrong-password")
            codes.append(code)
        self.assertEqual(codes[:3], [401, 401, 401])
        self.assertEqual(codes[3], 429)

    def test_unauthorized_and_static_turn(self) -> None:
        code, body, headers = self.request("GET", "/demo/turn")
        self.assertEqual(code, 401)
        payload = json.loads(body)
        self.assertNotIn("username", payload)
        self.assertNotIn("credential", payload)
        self.assertIn("no-store", headers.get("Cache-Control"))
        code, body, headers = self.request(
            "GET",
            "/demo/turn",
            headers={"Authorization": f"Bearer {TOKEN}", "X-Original-URI": "/demo/turn"},
        )
        self.assertEqual(code, 200)
        payload = json.loads(body)
        self.assertEqual(payload["mode"], "static")
        self.assertTrue(payload["temporary"])
        self.assertFalse(payload["minted"])
        self.assertFalse(payload["rest_api"])
        self.assertIsNone(payload["ttl"])
        self.assertEqual(payload["username"], "stage1user")
        self.assertEqual(payload["credential"], "stage1pass")
        self.assertIn("not minted", payload["note"])
        self.assertIn("no-store", headers.get("Cache-Control"))
        self.assertNotRegex(payload["username"], r"^[0-9]+:")

    def test_turn_missing_or_minted_does_not_invent_credentials(self) -> None:
        (self.conf / "turn.json").unlink()
        code, body, _ = self.request(
            "GET",
            "/demo/turn",
            headers={"Authorization": f"Bearer {TOKEN}", "X-Original-URI": "/demo/turn"},
        )
        self.assertEqual(code, 503)
        payload = json.loads(body)
        self.assertNotIn("username", payload)
        self.assertNotIn("credential", payload)
        self.assertFalse(payload["minted"])
        (self.conf / "turn.json").write_text(json.dumps({
            "mode": "rest",
            "minted": True,
            "username": "1999999999:demo",
            "credential": "pretendthisworks0001",
        }), encoding="utf-8")
        code, body, _ = self.request(
            "GET",
            "/demo/turn",
            headers={"Authorization": f"Bearer {TOKEN}", "X-Original-URI": "/demo/turn"},
        )
        self.assertEqual(code, 503)
        text = body.decode()
        self.assertNotIn("pretendthisworks0001", text)
        self.assertNotIn("1999999999", text)
        self.assertIn("not approved", text)

    def test_turn_rejects_ip_literals(self) -> None:
        (self.conf / "turn.json").write_text(json.dumps({
            "mode": "static",
            "username": "stage1user",
            "credential": "stage1pass",
            "uris": ["turn:217.77.3.65:3478?transport=udp"],
        }), encoding="utf-8")
        code, body, _ = self.request(
            "GET",
            "/demo/turn",
            headers={"Authorization": f"Bearer {TOKEN}", "X-Original-URI": "/demo/turn"},
        )
        self.assertEqual(code, 503)
        self.assertNotIn(b"stage1pass", body)

    def test_query_token_is_not_logged_or_reflected_on_login(self) -> None:
        code, body, headers = self.request(
            "GET",
            f"/demo/login?token={TOKEN}",
            capture=True,
        )
        self.assertEqual(code, 200)
        self.assertNotIn(TOKEN.encode(), body)
        self.assertIsNone(self.session_cookie(headers))
        self.assertNotIn(TOKEN, "\n".join(self.logs))

    def test_logout_clears_cookie(self) -> None:
        _, _, headers = self.login(PASSWORD)
        value = self.session_cookie(headers).split(";", 1)[0].split("=", 1)[1]
        code, _, cleared = self.request("POST", "/demo/logout", headers={"Cookie": f"wsdemo={value}"})
        self.assertEqual(code, 303)
        self.assertEqual(cleared.get("Location"), "/demo/login")
        cookie = self.session_cookie(cleared)
        self.assertIn("Max-Age=0", cookie)
        code, _, _ = self.request(
            "GET",
            "/_demo_auth",
            headers={"Cookie": cookie.split(";", 1)[0], "X-Original-URI": "/demo/"},
        )
        self.assertEqual(code, 401)

    def test_token_compare_visits_every_client(self) -> None:
        calls = {"n": 0}
        real = hmac.compare_digest

        def wrapped(left, right):
            calls["n"] += 1
            return real(left, right)

        hmac.compare_digest = wrapped
        try:
            self.assertEqual(demo_auth.match_token(OTHER), "vr-babelfish")
        finally:
            hmac.compare_digest = real
        self.assertGreaterEqual(calls["n"], 2)

    def test_healthz_has_no_secrets(self) -> None:
        code, body, _ = self.request("GET", "/healthz")
        self.assertEqual(code, 200)
        self.assertEqual(json.loads(body), {"ok": True})
        self.assertNotIn(TOKEN.encode(), body)

    def test_placeholder_config_is_rejected(self) -> None:
        scratch = Path(self._tmp.name) / "examples"
        scratch.mkdir()
        os.environ["DEMO_CONF_DIR"] = str(scratch)
        (scratch / "session.key").write_text((ROOT / "config" / "session.key.example").read_text(), encoding="utf-8")
        (scratch / "password").write_text((ROOT / "config" / "password.example").read_text(), encoding="utf-8")
        (scratch / "tokens").write_text((ROOT / "config" / "tokens.example").read_text(), encoding="utf-8")
        (scratch / "turn.json").write_text((ROOT / "config" / "turn.example.json").read_text(), encoding="utf-8")
        self.assertTrue(demo_auth.config_errors())
        code, _, _ = self.request("GET", "/demo/login")
        self.assertEqual(code, 503)


if __name__ == "__main__":
    unittest.main()
