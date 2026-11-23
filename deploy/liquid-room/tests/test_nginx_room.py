#!/usr/bin/env python3
"""nginx room locations: auth gate, credential stripping, gate header, TLS upstream verification."""
from __future__ import annotations

import base64
import hashlib
import json
import os
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOM_DIR = HERE.parent
DEMO = ROOM_DIR.parent / "webrtcsays-demo"
sys.path.insert(0, str(DEMO / "auth"))
sys.path.insert(0, str(DEMO / "tests"))

import demo_auth  # noqa: E402
from test_nginx_ws import masked_frame, read_frame  # noqa: E402

TOKEN = "a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6"
PASSWORD = "browserpass1234"
GATE = "0123456789abcdef" * 4
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
NGINX = shutil.which("nginx") or ("/usr/sbin/nginx" if Path("/usr/sbin/nginx").exists() else None)


def make_pki(root: Path) -> tuple[Path, Path, Path]:
    ca_key, ca_crt = root / "ca.key", root / "ca.crt"
    key, csr, crt, ext = root / "srv.key", root / "srv.csr", root / "srv.crt", root / "ext.cnf"
    run = lambda *a: subprocess.run(a, check=True, capture_output=True)  # noqa: E731
    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", str(ca_key), "-out", str(ca_crt),
        "-days", "2", "-subj", "/CN=test-ca", "-addext", "basicConstraints=critical,CA:TRUE",
        "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    run("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-keyout", str(key), "-out", str(csr),
        "-subj", "/CN=liquidserver.wilddolphin.us")
    ext.write_text("subjectAltName=DNS:liquidserver.wilddolphin.us\nextendedKeyUsage=serverAuth\n")
    run("openssl", "x509", "-req", "-in", str(csr), "-CA", str(ca_crt), "-CAkey", str(ca_key), "-CAcreateserial",
        "-out", str(crt), "-days", "2", "-extfile", str(ext))
    return ca_crt, crt, key


class TlsWsStub(threading.Thread):
    def __init__(self, crt: Path, key: Path) -> None:
        super().__init__(daemon=True)
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.load_cert_chain(str(crt), str(key))
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(8)
        self.port = self.sock.getsockname()[1]
        self.requests: list[str] = []
        self.sni: list[str | None] = []
        self.texts: list[str] = []
        self.stop_flag = threading.Event()
        self.ctx.sni_callback = lambda s, name, c: self.sni.append(name)

    def run(self) -> None:
        self.sock.settimeout(0.2)
        while not self.stop_flag.is_set():
            try:
                raw, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            threading.Thread(target=self.handle, args=(raw,), daemon=True).start()

    def handle(self, raw: socket.socket) -> None:
        raw.settimeout(3)
        try:
            conn = self.ctx.wrap_socket(raw, server_side=True)
        except (ssl.SSLError, OSError):
            raw.close()
            return
        try:
            data = b""
            while b"\r\n\r\n" not in data:
                chunk = conn.recv(4096)
                if not chunk:
                    return
                data += chunk
            head, rest = data.split(b"\r\n\r\n", 1)
            text = head.decode("iso-8859-1")
            self.requests.append(text)
            key = next((l.split(":", 1)[1].strip() for l in text.split("\r\n") if l.lower().startswith("sec-websocket-key:")), "")
            accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
            conn.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
            buf = rest
            while True:
                opcode, payload, buf = read_frame(conn, buf)
                if opcode == 0x1:
                    msg = payload.decode()
                    self.texts.append(msg)
                    if msg == "HELLO":
                        conn.sendall(b"\x81\x06200 OK")
                    elif msg == "BYE":
                        return
                elif opcode == 0x8:
                    return
        except (ConnectionError, socket.timeout, OSError, ssl.SSLError):
            return
        finally:
            conn.close()

    def close(self) -> None:
        self.stop_flag.set()
        self.sock.close()


@unittest.skipUnless(NGINX and shutil.which("openssl"), "nginx and openssl are required")
class NginxRoomTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        conf = self.root / "conf"
        conf.mkdir()
        os.environ.update({"DEMO_CONF_DIR": str(conf), "DEMO_TRUST_PROXY": "1", "DEMO_LOGIN_FLOOR_MS": "0",
                           "DEMO_LOGIN_LIMIT": "40", "DEMO_SESSION_TTL": "43200"})
        (conf / "session.key").write_text("k" * 48)
        (conf / "password").write_text(PASSWORD + "\n")
        (conf / "tokens").write_text(f"founder-iphone {TOKEN}\n")
        (conf / "turn.json").write_text(json.dumps({"mode": "static", "username": "u", "credential": "c",
                                                    "uris": ["turn:turn.wilddolphin.us:3478?transport=udp"]}))
        for name in ("session.key", "password", "tokens", "turn.json"):
            os.chmod(conf / name, 0o600)
        demo_auth.reset_rate_limits()
        self.auth = demo_auth.DemoServer(("127.0.0.1", 0))
        threading.Thread(target=self.auth.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True).start()
        ca, crt, key = make_pki(self.root)
        self.stub = TlsWsStub(crt, key)
        self.stub.start()
        self.logs = self.root / "logs"
        self.logs.mkdir()
        page_dir = self.root / "liquid-room"
        page_dir.mkdir()
        shutil.copyfile(ROOM_DIR / "page" / "index.html", page_dir / "index.html")
        for path in (self.root, page_dir):          # workers run as nobody when the suite runs as root
            os.chmod(path, 0o755)
        os.chmod(page_dir / "index.html", 0o644)
        for name in ("client_body", "proxy", "fastcgi", "uwsgi", "scgi"):
            (self.root / name).mkdir()
        gate_conf = self.root / "liquid-gate.conf"
        gate_conf.write_text(f'proxy_set_header X-Liquid-Gate "{GATE}";\n')
        demo_loc = (DEMO / "nginx" / "webrtcsays-demo-locations.conf").read_text()
        demo_loc = demo_loc.replace("127.0.0.1:8793", f"127.0.0.1:{self.auth.server_address[1]}")
        demo_loc = demo_loc.replace("/var/log/nginx", str(self.logs)).replace("/srv/webrtcsays-demo", str(self.root))
        room_loc = (ROOM_DIR / "nginx" / "liquid-room-locations.conf").read_text()
        room_loc = (room_loc.replace("/var/log/nginx", str(self.logs))
                    .replace("/srv/liquid-room", str(page_dir))
                    .replace("/etc/nginx/liquid-gate.conf", str(gate_conf))
                    .replace("/etc/ssl/certs/ca-certificates.crt", str(ca)))
        room_http = (ROOM_DIR / "nginx" / "liquid-room-http.conf").read_text().replace(
            "liquidserver.wilddolphin.us:443", f"127.0.0.1:{self.stub.port}")
        (self.root / "demo-loc.conf").write_text(demo_loc)
        (self.root / "room-loc.conf").write_text(room_loc)
        (self.root / "room-http.conf").write_text(room_http)
        shutil.copyfile(DEMO / "nginx" / "webrtcsays-demo-http.conf", self.root / "demo-http.conf")
        self.port = self.free_port()
        r = self.root
        (r / "nginx.conf").write_text(f"""
worker_processes 1;
error_log {self.logs / 'error.log'} warn;
pid {r / 'nginx.pid'};
events {{ worker_connections 64; }}
http {{
    include {r / 'demo-http.conf'};
    include {r / 'room-http.conf'};
    access_log off;
    client_body_temp_path {r / 'client_body'};
    proxy_temp_path {r / 'proxy'};
    fastcgi_temp_path {r / 'fastcgi'};
    uwsgi_temp_path {r / 'uwsgi'};
    scgi_temp_path {r / 'scgi'};
    server {{
        listen 127.0.0.1:{self.port};
        server_name localhost;
        include {r / 'demo-loc.conf'};
        include {r / 'room-loc.conf'};
        location / {{ return 200 "gateway\\n"; }}
    }}
}}
""")
        self.proc = subprocess.Popen([NGINX, "-p", str(r), "-c", str(r / "nginx.conf"), "-g", "daemon off;"],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        deadline = time.time() + 3
        while True:
            if self.proc.poll() is not None:
                self.fail(self.proc.stderr.read().decode())
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
                break
            except OSError:
                if time.time() > deadline:
                    self.fail("nginx did not listen")
                time.sleep(0.05)

        class NoRedirect(urllib.request.HTTPRedirectHandler):
            def http_error_302(self, req, fp, code, msg, headers):
                raise urllib.error.HTTPError(req.full_url, code, msg, headers, fp)
            http_error_301 = http_error_303 = http_error_302

        self.opener = urllib.request.build_opener(NoRedirect)

    def tearDown(self) -> None:
        self.proc.terminate()
        try:
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        for s in (self.proc.stdout, self.proc.stderr):
            s and s.close()
        self.stub.close()
        self.auth.shutdown()
        self.auth.server_close()
        self.tmp.cleanup()

    def free_port(self) -> int:
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        return port

    def http(self, path: str, headers: dict | None = None):
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}{path}", headers=headers or {})
        try:
            with self.opener.open(req, timeout=3) as resp:
                return resp.status, resp.read(), resp.headers
        except urllib.error.HTTPError as exc:
            return exc.code, exc.read(), exc.headers

    def ws(self, path: str, headers: list[tuple[str, str]], send: list[str]) -> tuple[str, list[str]]:
        sock = socket.create_connection(("127.0.0.1", self.port), timeout=3)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (f"GET {path} HTTP/1.1\r\nHost: rtc.wilddolphin.us\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: {key}\r\n")
        req += "".join(f"{k}: {v}\r\n" for k, v in headers) + "\r\n"
        sock.sendall(req.encode())
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = sock.recv(4096)
            if not chunk:
                break
            data += chunk
        head, _, buf = data.partition(b"\r\n\r\n")
        status = head.split(b"\r\n", 1)[0].decode()
        got: list[str] = []
        if status.startswith("HTTP/1.1 101"):
            for text in send:
                sock.sendall(masked_frame(0x1, text.encode()))
            try:
                opcode, payload, buf = read_frame(sock, buf)
                if opcode == 0x1:
                    got.append(payload.decode())
            except (ConnectionError, socket.timeout):
                pass
            sock.sendall(masked_frame(0x1, b"BYE"))
            time.sleep(0.1)
        sock.close()
        return status, got

    def test_page_needs_login_and_keeps_the_room(self) -> None:
        code, _, headers = self.http("/demo/room/room101")
        self.assertEqual(code, 302)
        self.assertTrue(headers["Location"].endswith(":%d/demo/login?next=/demo/room/room101" % self.port), headers["Location"])
        code, body, headers = self.http("/demo/room/room101", {"Authorization": f"Bearer {TOKEN}"})
        self.assertEqual(code, 200)
        self.assertIn(b"<title>LiquidCall room</title>", body)
        self.assertEqual(headers["Cache-Control"], "no-store, private")
        code, _, headers = self.http("/demo/room")
        self.assertEqual((code, headers["Location"].rsplit(":%d" % self.port, 1)[-1]), (301, "/demo/room/room101"))

    def test_socket_without_credential_never_reaches_liquidserver(self) -> None:
        for headers in ([], [("Authorization", "Bearer wrongwrongwrongwrong")], [("Cookie", "wsdemo=v1.1.x.y")]):
            status, _ = self.ws("/demo/room/ws/room101", headers, [])
            self.assertIn(" 401 ", status + " ")
        status, _ = self.ws("/demo/room/ws/room101?token=wrongwrongwrongwrong", [], [])
        self.assertIn(" 401 ", status + " ")
        self.assertEqual(self.stub.requests, [])

    def test_bearer_socket_is_stripped_and_gated(self) -> None:
        status, got = self.ws("/demo/room/ws/room101?x=1", [("Authorization", f"Bearer {TOKEN}"),
                                                             ("Cookie", "a=b"), ("X-Liquid-Gate", "forged")], ["HELLO"])
        self.assertTrue(status.startswith("HTTP/1.1 101"), status)
        self.assertEqual(got, ["200 OK"])
        time.sleep(0.1)
        self.assertEqual(len(self.stub.requests), 1)
        req = self.stub.requests[0]
        lines = req.split("\r\n")
        self.assertEqual(lines[0], "GET /ws/room101 HTTP/1.1")
        hdr = {l.split(":", 1)[0].lower(): l.split(":", 1)[1].strip() for l in lines[1:] if ":" in l}
        self.assertEqual(hdr.get("host"), "liquidserver.wilddolphin.us")
        gates = [l for l in lines if l.lower().startswith("x-liquid-gate:")]
        self.assertEqual(gates, [f"X-Liquid-Gate: {GATE}"], "client cannot forge or add a gate header")
        for name in ("authorization", "cookie", "x-demo-token"):
            self.assertIn(hdr.get(name, ""), ("",), name)
        self.assertNotIn(TOKEN, req)
        self.assertEqual(self.stub.sni, ["liquidserver.wilddolphin.us"])
        self.assertEqual(self.stub.texts[:1], ["HELLO"])

    def test_query_token_is_not_forwarded_or_logged(self) -> None:
        status, got = self.ws(f"/demo/room/ws/room101?token={TOKEN}", [], ["HELLO"])
        self.assertTrue(status.startswith("HTTP/1.1 101"), status)
        self.assertEqual(got, ["200 OK"])
        time.sleep(0.2)
        self.assertNotIn(TOKEN, "".join(self.stub.requests))
        logs = "".join(p.read_text(errors="replace") for p in self.logs.iterdir())
        self.assertNotIn(TOKEN, logs)
        self.assertNotIn(GATE, logs)
        self.assertIn("/demo/room/ws/room101", logs)

    def test_reserved_and_malformed_paths_do_not_proxy(self) -> None:
        for path in ("/demo/room/ws", "/demo/room/ws/", "/demo/room/ws/room101/x", "/demo/room/ws/-x"):
            status, _ = self.ws(path, [("Authorization", f"Bearer {TOKEN}")], [])
            self.assertFalse(status.startswith("HTTP/1.1 101"), path)
        self.assertEqual(self.stub.requests, [])


if __name__ == "__main__":
    unittest.main()
