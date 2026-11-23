#!/usr/bin/env python3
"""Drive tools/signaling_ref.py over real sockets with the SIGNALING.md vectors."""
from __future__ import annotations

import asyncio
import base64
import json
import os
import socket
import sys
import threading
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tools"))
import signaling_ref as ref  # noqa: E402

GATE = "ab" * 32
VECTORS = json.loads((HERE / "vectors.json").read_text(encoding="utf-8"))["vectors"]


def masked(opcode: int, payload: bytes) -> bytes:
    mask = os.urandom(4)
    n = len(payload)
    if n < 126:
        head = bytes([0x80 | opcode, 0x80 | n])
    elif n <= 65535:
        head = bytes([0x80 | opcode, 0x80 | 126]) + n.to_bytes(2, "big")
    else:
        head = bytes([0x80 | opcode, 0x80 | 127]) + n.to_bytes(8, "big")
    return head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload))


class Client:
    def __init__(self, port: int, path: str = "/ws/room101", gate: str | None = GATE) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=2)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (f"GET {path} HTTP/1.1\r\nHost: liquidserver.test\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: {key}\r\n")
        if gate is not None:
            req += f"X-Liquid-Gate: {gate}\r\n"
        self.sock.sendall((req + "\r\n").encode())
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            data += chunk
        head, _, self.buf = data.partition(b"\r\n\r\n")
        self.status = head.split(b"\r\n", 1)[0].decode()

    def send(self, text: str) -> None:
        self.sock.sendall(masked(0x1, text.encode()))

    def recv(self, timeout: float = 1.0) -> tuple[int, bytes] | None:
        self.sock.settimeout(timeout)

        def take(n: int) -> bool:
            while len(self.buf) < n:
                try:
                    chunk = self.sock.recv(65536)
                except (socket.timeout, OSError):
                    return False
                if not chunk:
                    return False
                self.buf += chunk
            return True

        if not take(2):
            return None
        op, n = self.buf[0] & 0x0F, self.buf[1] & 0x7F
        off = 2
        if n == 126:
            if not take(4):
                return None
            n, off = int.from_bytes(self.buf[2:4], "big"), 4
        elif n == 127:
            if not take(10):
                return None
            n, off = int.from_bytes(self.buf[2:10], "big"), 10
        if self.buf[1] & 0x80:
            raise AssertionError("server frames must not be masked")
        if not take(off + n):
            return None
        payload, self.buf = self.buf[off:off + n], self.buf[off + n:]
        return op, payload

    def text(self, timeout: float = 1.0) -> str | None:
        while True:
            frame = self.recv(timeout)
            if frame is None:
                return None
            if frame[0] == 0x1:
                return frame[1].decode()
            if frame[0] == 0x8:
                return None

    def close(self) -> None:
        self.sock.close()


class RefServerTests(unittest.TestCase):
    loaded = ref.VOICE_IDS

    def setUp(self) -> None:
        self.loop = asyncio.new_event_loop()
        self.server = ref.Server(GATE, self.loaded)
        ready = threading.Event()

        def run() -> None:
            asyncio.set_event_loop(self.loop)
            self.srv = self.loop.run_until_complete(ref.serve(self.server, "127.0.0.1", 0))
            self.port = self.srv.sockets[0].getsockname()[1]
            ready.set()
            self.loop.run_forever()

        self.thread = threading.Thread(target=run, daemon=True)
        self.thread.start()
        ready.wait(2)

    def tearDown(self) -> None:
        async def cancel_all() -> None:
            self.srv.close()
            tasks = [t for t in asyncio.all_tasks() if t is not asyncio.current_task()]
            for t in tasks:
                t.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)

        asyncio.run_coroutine_threadsafe(cancel_all(), self.loop).result(2)
        self.loop.call_soon_threadsafe(self.loop.stop)
        self.thread.join(2)
        self.loop.close()

    def run_vector(self, v: dict) -> list[str]:
        c = Client(self.port, f"/ws/{v['room']}")
        self.assertTrue(c.status.startswith("HTTP/1.1 101"), c.status)
        answers = []
        for text, expect in v["steps"]:
            c.send(text)
            if expect is None:
                self.assertIsNone(c.text(timeout=0.2), f"{v['name']}: no reply expected to {text[:12]}")
                continue
            got = c.text()
            self.assertIsNotNone(got, f"{v['name']}: no reply to {text[:12]}")
            self.assertTrue(got.startswith(expect), f"{v['name']}: {got[:40]!r} for {text[:12]}")
            if got.startswith("ANSWER:"):
                answers.append(got[len("ANSWER:"):])
        if v.get("closed"):
            self.assertIsNone(c.text(timeout=0.5), f"{v['name']}: socket should close")
        c.close()
        return answers

    def test_vectors(self) -> None:
        for v in VECTORS:
            with self.subTest(v["name"]):
                answers = self.run_vector(v)
                if "answer_has" in v:
                    for needle in v["answer_has"]:
                        self.assertIn(needle, answers[-1], v["name"])
                for needle in v.get("answer_lacks", []):
                    self.assertNotIn(needle, answers[-1], v["name"])
                for needle in v.get("first_answer_has", []):
                    self.assertIn(needle, answers[0], v["name"])
                time.sleep(0.05)

    def test_gate_query_and_path(self) -> None:
        for path, gate, status in [
            ("/ws/room101", None, "401"),
            ("/ws/room101", "cd" * 32, "401"),
            ("/ws/room101", GATE[:-1], "401"),
            ("/ws/room101?token=x", GATE, "400"),
            ("/ws/ws", GATE, "404"),
            ("/ws/", GATE, "404"),
            ("/ws/room101/x", GATE, "404"),
            ("/demo/room/ws/room101", GATE, "404"),
            ("/ws/-room", GATE, "404"),
        ]:
            c = Client(self.port, path, gate)
            self.assertIn(f" {status} ", c.status + " ", (path, gate))
            c.close()

    def test_room_is_two_people_and_isolated(self) -> None:
        a, b, c = (Client(self.port) for _ in range(3))
        for x in (a, b):
            x.send("HELLO")
            self.assertEqual(x.text(), "200 OK")
        c.send("HELLO")
        self.assertEqual(c.text(), "486 Busy Here")
        self.assertIsNone(c.text(timeout=0.5))
        d = Client(self.port, "/ws/room202")
        d.send("HELLO")
        self.assertEqual(d.text(), "200 OK")
        a.send("BYE")
        self.assertEqual(a.text(), "200 OK")
        time.sleep(0.1)
        e = Client(self.port)
        e.send("HELLO")
        self.assertEqual(e.text(), "200 OK")
        for x in (a, b, c, d, e):
            x.close()

    def test_ping_gets_pong(self) -> None:
        c = Client(self.port)
        c.sock.sendall(masked(0x9, b"hi"))
        self.assertEqual(c.recv(), (0xA, b"hi"))
        c.close()

    def test_unmasked_client_frame_closes(self) -> None:
        c = Client(self.port)
        c.sock.sendall(bytes([0x81, 5]) + b"HELLO")
        self.assertIsNone(c.text(timeout=0.5))
        c.close()

    def test_log_has_verbs_only(self) -> None:
        self.run_vector(VECTORS[0])
        time.sleep(0.1)
        joined = "\n".join(self.server.log)
        self.assertIn("msg room=room101", joined)
        for secret in ("opus/48000", "candidate:", "192.0.2.10", "room_name", GATE):
            self.assertNotIn(secret, joined)


class VoiceFallbackTests(RefServerTests):
    loaded = ("me", "female")

    def test_vectors(self) -> None:  # the full set assumes all voices are loaded
        pass

    def test_unloaded_voice_answers_me(self) -> None:
        offer = next(v for v in VECTORS if v["name"] == "legacy_payload_name")
        answers = self.run_vector(offer)
        self.assertIn("a=x-liquidcall-voice:96 me", answers[-1])
        self.assertIn("a=x-liquidcall-voices:96 me,female", answers[-1])
        self.assertNotIn("male\r\n", answers[-1].replace("female", ""))


class SdpRuleTests(unittest.TestCase):
    def test_gate_file_format(self) -> None:
        import tempfile
        with tempfile.NamedTemporaryFile("w", delete=False) as f:
            f.write("not-hex\n")
        with self.assertRaises(SystemExit):
            ref.load_gate(f.name)
        Path(f.name).write_text(GATE + "\n")
        self.assertEqual(ref.load_gate(f.name), GATE)
        os.unlink(f.name)

    def test_non_loopback_bind_refused(self) -> None:
        with self.assertRaises(SystemExit):
            asyncio.run(ref.serve(ref.Server(GATE), "0.0.0.0", 0))


if __name__ == "__main__":
    unittest.main()
