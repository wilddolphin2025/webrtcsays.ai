#!/usr/bin/env python3
"""Nginx gate, WebSocket path, and directcall text-frame behavior."""

from __future__ import annotations

import base64
import hashlib
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
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

TOKEN = "a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6"
PASSWORD = "browserpass1234"
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def masked_frame(opcode: int, payload: bytes) -> bytes:
    mask = os.urandom(4)
    header = bytes([0x80 | opcode, 0x80 | len(payload)])
    masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
    return header + mask + masked


def read_frame(sock: socket.socket, buf: bytes) -> tuple[int, bytes, bytes]:
    def take(size: int) -> None:
        nonlocal buf
        while len(buf) < size:
            chunk = sock.recv(4096)
            if not chunk:
                raise ConnectionError("closed")
            buf += chunk

    take(2)
    opcode = buf[0] & 0x0F
    masked = bool(buf[1] & 0x80)
    length = buf[1] & 0x7F
    offset = 2
    if length == 126:
        take(4)
        length = int.from_bytes(buf[2:4], "big")
        offset = 4
    elif length == 127:
        take(10)
        length = int.from_bytes(buf[2:10], "big")
        offset = 10
    if masked:
        take(offset + 4 + length)
        mask = buf[offset:offset + 4]
        start = offset + 4
        payload = bytes(byte ^ mask[index % 4] for index, byte in enumerate(buf[start:start + length]))
        return opcode, payload, buf[start + length:]
    take(offset + length)
    return opcode, buf[offset:offset + length], buf[offset + length:]


class WsStub(threading.Thread):
    def __init__(self) -> None:
        super().__init__(daemon=True)
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(16)
        self.port = self.sock.getsockname()[1]
        self.requests: list[str] = []
        self.texts: list[str] = []
        self.opcodes: list[int] = []
        self.pongs: list[bytes] = []
        self.stop_flag = threading.Event()

    def run(self) -> None:
        self.sock.settimeout(0.2)
        while not self.stop_flag.is_set():
            try:
                conn, _addr = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            threading.Thread(target=self.handle, args=(conn,), daemon=True).start()

    def handle(self, conn: socket.socket) -> None:
        conn.settimeout(3)
        try:
            data = b""
            while b"\r\n\r\n" not in data:
                chunk = conn.recv(4096)
                if not chunk:
                    return
                data += chunk
            head, rest = data.split(b"\r\n\r\n", 1)
            text = head.decode("iso-8859-1", "replace")
            self.requests.append(text)
            key = ""
            for line in text.split("\r\n")[1:]:
                if line.lower().startswith("sec-websocket-key:"):
                    key = line.split(":", 1)[1].strip()
            if not key:
                return
            accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
            response = (
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
            )
            conn.sendall(response.encode())
            buf = rest
            sent_ping = False
            while True:
                opcode, payload, buf = read_frame(conn, buf)
                self.opcodes.append(opcode)
                if opcode == 0x1:
                    message = payload.decode("utf-8", "replace")
                    self.texts.append(message)
                    if message == "HELLO:Slim":
                        conn.sendall(b"\x81\x06" + b"200 OK")
                        if not sent_ping:
                            conn.sendall(b"\x89\x01p")
                            sent_ping = True
                    elif message == "BYE":
                        return
                elif opcode == 0xA:
                    self.pongs.append(payload)
                elif opcode == 0x8:
                    return
        except (ConnectionError, socket.timeout, OSError):
            return
        finally:
            conn.close()

    def close(self) -> None:
        self.stop_flag.set()
        self.sock.close()


class NginxWsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.conf = self.root / "conf"
        self.conf.mkdir()
        os.environ["DEMO_CONF_DIR"] = str(self.conf)
        os.environ["DEMO_TRUST_PROXY"] = "1"
        os.environ["DEMO_LOGIN_FLOOR_MS"] = "0"
        os.environ["DEMO_LOGIN_LIMIT"] = "40"
        os.environ["DEMO_SESSION_TTL"] = "43200"
        (self.conf / "session.key").write_text("k" * 48, encoding="utf-8")
        (self.conf / "password").write_text(PASSWORD + "\n", encoding="utf-8")
        (self.conf / "tokens").write_text(f"founder-iphone {TOKEN}\n", encoding="utf-8")
        (self.conf / "turn.json").write_text(json.dumps({
            "mode": "static",
            "username": "stage1user",
            "credential": "stage1pass",
            "uris": ["turn:turn.wilddolphin.us:3478?transport=udp"],
        }), encoding="utf-8")
        for name in ("session.key", "password", "tokens", "turn.json"):
            os.chmod(self.conf / name, 0o600)
        demo_auth.reset_rate_limits()
        self.auth = demo_auth.DemoServer(("127.0.0.1", 0))
        self.auth_thread = threading.Thread(
            target=self.auth.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True
        )
        self.auth_thread.start()
        self.auth_port = self.auth.server_address[1]
        self.stub = WsStub()
        self.stub.start()
        self.page_dir = self.root / "page"
        self.page_dir.mkdir()
        shutil.copyfile(ROOT / "page" / "index.html", self.page_dir / "index.html")
        self.logs = self.root / "logs"
        self.logs.mkdir()
        for name in ("client_body", "proxy", "fastcgi", "uwsgi", "scgi"):
            (self.root / name).mkdir()
        self.locations = (ROOT / "nginx" / "webrtcsays-demo-locations.conf").read_text(encoding="utf-8")
        self.locations = self.locations.replace("127.0.0.1:8793", f"127.0.0.1:{self.auth_port}")
        self.locations = self.locations.replace("127.0.0.1:3459", f"127.0.0.1:{self.stub.port}")
        self.locations = self.locations.replace("/var/log/nginx", str(self.logs))
        self.locations = self.locations.replace("/srv/webrtcsays-demo", str(self.page_dir))
        (self.root / "locations.conf").write_text(self.locations, encoding="utf-8")
        http_conf = (ROOT / "nginx" / "webrtcsays-demo-http.conf").read_text(encoding="utf-8")
        (self.root / "http.conf").write_text(http_conf, encoding="utf-8")
        self.port = self.free_port()
        nginx_conf = f"""
worker_processes 1;
error_log {self.logs / "error.log"} info;
pid {self.root / "nginx.pid"};
events {{ worker_connections 64; }}
http {{
    include {self.root / "http.conf"};
    access_log off;
    client_body_temp_path {self.root / "client_body"};
    proxy_temp_path {self.root / "proxy"};
    fastcgi_temp_path {self.root / "fastcgi"};
    uwsgi_temp_path {self.root / "uwsgi"};
    scgi_temp_path {self.root / "scgi"};
    server {{
        listen 127.0.0.1:{self.port};
        server_name localhost;
        include {self.root / "locations.conf"};
        location / {{ return 200 "gateway\\n"; }}
        location /ws/ {{ return 200 "signal\\n"; }}
        location /babelfish/healthz {{ return 200 "babel\\n"; }}
    }}
}}
"""
        (self.root / "nginx.conf").write_text(nginx_conf, encoding="utf-8")
        self.proc = subprocess.Popen(
            ["nginx", "-p", str(self.root), "-c", str(self.root / "nginx.conf"), "-g", "daemon off;"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.wait_for_port()

        class NoRedirect(urllib.request.HTTPRedirectHandler):
            def http_error_302(self, req, fp, code, msg, headers):
                raise urllib.error.HTTPError(req.full_url, code, msg, headers, fp)

            http_error_301 = http_error_303 = http_error_307 = http_error_308 = http_error_302

        self.opener = urllib.request.build_opener(NoRedirect)

    def tearDown(self) -> None:
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        if self.proc.stdout:
            self.proc.stdout.close()
        if self.proc.stderr:
            self.proc.stderr.close()
        self.stub.close()
        self.auth.shutdown()
        self.auth.server_close()
        self.tmp.cleanup()

    def free_port(self) -> int:
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
        sock.close()
        return port

    def wait_for_port(self) -> None:
        deadline = time.time() + 3
        while time.time() < deadline:
            if self.proc.poll() is not None:
                _out, err = self.proc.communicate()
                self.fail(err.decode("utf-8", "replace"))
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.2):
                    return
            except OSError:
                time.sleep(0.05)
        self.fail("nginx did not listen")

    def http(self, method, path, body=None, headers=None):
        data = body.encode() if isinstance(body, str) else body
        req = urllib.request.Request(
            f"http://127.0.0.1:{self.port}{path}", data=data, method=method, headers=headers or {}
        )
        try:
            with self.opener.open(req, timeout=3) as resp:
                return resp.status, resp.read(), resp.headers
        except urllib.error.HTTPError as exc:
            return exc.code, exc.read(), exc.headers

    def ws(self, path: str, headers: list[tuple[str, str]], frames: list[bytes]) -> tuple[str, list[tuple[int, bytes]]]:
        sock = socket.create_connection(("127.0.0.1", self.port), timeout=3)
        sock.settimeout(3)
        key = base64.b64encode(os.urandom(16)).decode()
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{self.port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
        )
        for name, value in headers:
            request += f"{name}: {value}\r\n"
        request += "\r\n"
        sock.sendall(request.encode())
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = sock.recv(4096)
            if not chunk:
                break
            data += chunk
        head, rest = data.split(b"\r\n\r\n", 1)
        status = head.decode("iso-8859-1", "replace").split("\r\n", 1)[0]
        messages: list[tuple[int, bytes]] = []
        if not status.startswith("HTTP/1.1 101"):
            sock.close()
            return status, messages
        for frame in frames:
            sock.sendall(frame)
        buf = rest
        deadline = time.time() + 2
        while time.time() < deadline and len(messages) < 2:
            try:
                opcode, payload, buf = read_frame(sock, buf)
            except (ConnectionError, socket.timeout):
                break
            messages.append((opcode, payload))
            if opcode == 0x9:
                sock.sendall(masked_frame(0xA, payload))
        sock.sendall(masked_frame(0x1, b"BYE"))
        time.sleep(0.1)
        sock.close()
        return status, messages

    def test_unauthorized_ws_never_reaches_upstream(self) -> None:
        before = len(self.stub.requests)
        status, _messages = self.ws("/demo/ws", [], [])
        self.assertTrue(status.startswith("HTTP/1.1 401"), status)
        time.sleep(0.1)
        self.assertEqual(len(self.stub.requests), before)
        status, _messages = self.ws("/ws?token=not-a-real-token", [], [])
        self.assertTrue(status.startswith("HTTP/1.1 401"), status)
        self.assertEqual(len(self.stub.requests), before)

    def test_bearer_rewrites_path_and_speaks_text_frames(self) -> None:
        status, messages = self.ws(
            "/demo/ws",
            [("Authorization", f"Bearer {TOKEN}")],
            [masked_frame(0x1, b"HELLO:Slim")],
        )
        self.assertTrue(status.startswith("HTTP/1.1 101"), status)
        self.assertTrue(self.stub.requests, "upstream saw no handshake")
        upstream = self.stub.requests[-1]
        self.assertTrue(upstream.startswith("GET / HTTP/1.1"), upstream.split("\r\n", 1)[0])
        self.assertNotIn(TOKEN, upstream)
        self.assertNotIn("?", upstream.split("\r\n", 1)[0])
        self.assertIn("HELLO:Slim", self.stub.texts)
        self.assertIn((0x1, b"200 OK"), messages)
        self.assertTrue(any(opcode == 0x9 for opcode, _payload in messages))
        self.assertIn(b"p", self.stub.pongs)
        self.assertIn("BYE", self.stub.texts)

    def test_query_token_is_not_forwarded_or_logged(self) -> None:
        status, messages = self.ws(
            f"/demo/ws?token={TOKEN}",
            [],
            [masked_frame(0x1, b"HELLO:Slim")],
        )
        self.assertTrue(status.startswith("HTTP/1.1 101"), status)
        upstream = self.stub.requests[-1]
        self.assertTrue(upstream.startswith("GET / HTTP/1.1"), upstream.split("\r\n", 1)[0])
        self.assertNotIn(TOKEN, upstream)
        self.assertIn((0x1, b"200 OK"), messages)
        legacy, _messages = self.ws(
            f"/ws?token={TOKEN}",
            [],
            [masked_frame(0x1, b"HELLO:Slim")],
        )
        self.assertTrue(legacy.startswith("HTTP/1.1 101"), legacy)
        self.assertTrue(self.stub.requests[-1].startswith("GET / HTTP/1.1"))
        self.assertNotIn(TOKEN, self.stub.requests[-1])
        auth_log = (self.logs / "demo-auth.log").read_text(encoding="utf-8")
        ws_log = (self.logs / "demo-ws.log").read_text(encoding="utf-8")
        error_log = (self.logs / "error.log").read_text(encoding="utf-8", errors="replace")
        for label, text in (("auth", auth_log), ("ws", ws_log), ("error", error_log)):
            self.assertNotIn(TOKEN, text, label)
        self.assertIn("/demo/ws", ws_log)
        self.assertNotIn("?", ws_log)

    def test_length_prefix_is_not_a_text_frame(self) -> None:
        before = len(self.stub.texts)
        status, messages = self.ws(
            "/demo/ws",
            [("Authorization", f"Bearer {TOKEN}")],
            [b"\x00\x0aHELLO:Slim"],
        )
        self.assertTrue(status.startswith("HTTP/1.1 101"), status)
        self.assertNotIn("HELLO:Slim", self.stub.texts[before:])
        self.assertFalse(any(payload == b"200 OK" for _opcode, payload in messages))

    def test_page_redirect_drops_query_token(self) -> None:
        code, body, headers = self.http("GET", f"/demo/?token={TOKEN}")
        self.assertEqual(code, 302)
        location = urllib.parse.urlsplit(headers.get("Location"))
        self.assertEqual(location.path, "/demo/login")
        self.assertEqual(urllib.parse.parse_qs(location.query), {"next": ["/demo/"]})
        self.assertNotIn(TOKEN, headers.get("Location"))
        self.assertNotIn(TOKEN.encode(), body)
        code, _body, headers = self.http("GET", f"/demo?token={TOKEN}")
        self.assertEqual(code, 301)
        self.assertNotIn(TOKEN, headers.get("Location"))
        self.assertTrue(urllib.parse.urlsplit(headers.get("Location")).path.endswith("/demo/"))
        auth_log = (self.logs / "demo-auth.log").read_text(encoding="utf-8")
        error_log = (self.logs / "error.log").read_text(encoding="utf-8", errors="replace")
        self.assertNotIn(TOKEN, auth_log)
        self.assertNotIn(TOKEN, error_log)

    def test_turn_and_page_and_untouched_routes(self) -> None:
        code, body, _headers = self.http("GET", "/demo/turn")
        self.assertIn(code, (302, 401))
        self.assertNotIn(b"stage1pass", body)
        code, body, headers = self.http(
            "GET",
            "/demo/turn",
            headers={"Authorization": f"Bearer {TOKEN}"},
        )
        self.assertEqual(code, 200, body)
        payload = json.loads(body)
        self.assertEqual(payload["mode"], "static")
        self.assertFalse(payload["minted"])
        self.assertTrue(payload["temporary"])
        self.assertEqual(payload["credential"], "stage1pass")
        self.assertEqual(headers.get_all("Cache-Control"), ["no-store, private"])
        code, body, _headers = self.http(
            "GET",
            "/demo/",
            headers={"Authorization": f"Bearer {TOKEN}"},
        )
        self.assertEqual(code, 200, body[:200])
        self.assertIn(b"room101", body)
        code, body, _headers = self.http("GET", "/")
        self.assertEqual(body, b"gateway\n")
        code, body, _headers = self.http("GET", "/ws/")
        self.assertEqual(body, b"signal\n")
        code, body, _headers = self.http("GET", "/babelfish/healthz")
        self.assertEqual(body, b"babel\n")
        code, _body, _headers = self.http("GET", "/_demo_auth")
        self.assertEqual(code, 404)

    def test_login_through_nginx_sets_cookie_without_open_redirect(self) -> None:
        code, body, headers = self.http("GET", "/demo/login?next=https://evil.example")
        self.assertEqual(code, 200)
        self.assertNotIn(b"evil.example", body)
        text = body.decode()
        csrf = text.split('name="csrf" value="', 1)[1].split('"', 1)[0]
        csrf_cookie = ""
        for item in headers.get_all("Set-Cookie") or []:
            if item.startswith("wsdemo_csrf="):
                csrf_cookie = item.split(";", 1)[0]
        form = urllib.parse.urlencode({
            "credential": PASSWORD,
            "csrf": csrf,
            "next": "//evil.example",
        })
        code, _body, headers = self.http(
            "POST",
            "/demo/login",
            form,
            {
                "Content-Type": "application/x-www-form-urlencoded",
                "Cookie": csrf_cookie,
                "Origin": f"http://127.0.0.1:{self.port}",
            },
        )
        self.assertEqual(code, 303)
        location = urllib.parse.urlsplit(headers.get("Location"))
        self.assertEqual(location.path, "/demo/")
        self.assertNotIn("evil.example", headers.get("Location"))
        session = ""
        for item in headers.get_all("Set-Cookie") or []:
            if item.startswith("wsdemo="):
                session = item.split(";", 1)[0]
        self.assertIn("Secure", " ".join(headers.get_all("Set-Cookie") or []))
        code, body, _headers = self.http("GET", "/demo/", headers={"Cookie": session})
        self.assertEqual(code, 200)
        self.assertIn(b"Slim", body)


if __name__ == "__main__":
    unittest.main()
