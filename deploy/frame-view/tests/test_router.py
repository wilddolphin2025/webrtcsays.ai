#!/usr/bin/env python3
"""Local checks for the Frame viewer router. No network beyond loopback."""

from __future__ import annotations

import asyncio
import base64
import hashlib
import os
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import router as frame_router

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class SocketClient:
    def __init__(self, reader, writer, room: str) -> None:
        self.reader = reader
        self.writer = writer
        self.room = room

    async def send(self, text: str) -> None:
        payload = text.encode("utf-8")
        mask = os.urandom(4)
        frame = bytearray([0x81, 0x80 | len(payload)])
        frame += mask
        frame += bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
        self.writer.write(frame)
        await self.writer.drain()

    async def recv(self) -> str:
        _opcode, payload = await self.recv_frame()
        return payload.decode("utf-8")

    async def recv_frame(self) -> tuple[int, bytes]:
        header = await asyncio.wait_for(self.reader.readexactly(2), timeout=2)
        opcode = header[0] & 0x0F
        masked = (header[1] & 0x80) != 0
        length = header[1] & 0x7F
        if length == 126:
            length = int.from_bytes(await self.reader.readexactly(2), "big")
        mask = await self.reader.readexactly(4) if masked else b""
        payload = await self.reader.readexactly(length) if length else b""
        if masked:
            payload = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
        return opcode, payload

    async def send_pong(self, payload: bytes = b"") -> None:
        mask = os.urandom(4)
        frame = bytearray([0x8A, 0x80 | len(payload)])
        frame += mask
        frame += bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
        self.writer.write(frame)
        await self.writer.drain()


async def open_client(port: int, room: str, auth: str = "ok", query: str = "") -> SocketClient:
    reader, writer = await asyncio.open_connection("127.0.0.1", port)
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    path = f"/demo/view/ws/{room}{query}"
    request = (
        f"GET {path} HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        f"X-Demo-Auth: {auth}\r\n\r\n"
    )
    writer.write(request.encode("ascii"))
    await writer.drain()
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = await asyncio.wait_for(reader.read(4096), timeout=2)
        if not chunk:
            break
        data += chunk
    status = data.split(b"\r\n", 1)[0].decode("latin1")
    if not status.startswith("HTTP/1.1 101"):
        writer.close()
        raise AssertionError(status)
    accept_line = [line for line in data.decode("latin1").split("\r\n") if line.lower().startswith("sec-websocket-accept:")]
    digest = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    if not accept_line or digest not in accept_line[0]:
        raise AssertionError("bad accept")
    return SocketClient(reader, writer, room)


class RouterTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self._idle = frame_router.IDLE_TIMEOUT
        self._ping = frame_router.PING_INTERVAL
        frame_router.ROUTER = frame_router.Router()
        self.server = await frame_router.serve("127.0.0.1", 0)
        self.port = self.server.sockets[0].getsockname()[1]

    async def asyncTearDown(self) -> None:
        frame_router.IDLE_TIMEOUT = self._idle
        frame_router.PING_INTERVAL = self._ping
        self.server.close()
        await self.server.wait_closed()

    async def test_rejects_missing_auth_and_query(self) -> None:
        reader, writer = await asyncio.open_connection("127.0.0.1", self.port)
        writer.write(b"GET /demo/view/ws/frame101 HTTP/1.1\r\nHost: localhost\r\n\r\n")
        await writer.drain()
        data = await reader.read(128)
        self.assertTrue(data.startswith(b"HTTP/1.1 401"))
        writer.close()
        with self.assertRaises(AssertionError):
            await open_client(self.port, "frame101", query="?token=secret")

    async def test_pairs_offer_answer_and_isolates_rooms(self) -> None:
        viewer = await open_client(self.port, "frame101")
        other = await open_client(self.port, "other-room")
        caller = await open_client(self.port, "frame101")
        await viewer.send("VIEW")
        await other.send("VIEW")
        await caller.send("HELLO:viewer")
        self.assertEqual(await viewer.recv(), "HELLO:viewer")
        leaked = asyncio.create_task(other.recv())
        done, _pending = await asyncio.wait({leaked}, timeout=0.2)
        self.assertFalse(done)
        leaked.cancel()
        await viewer.send("200 OK")
        self.assertEqual(await caller.recv(), "200 OK")
        await caller.send("OFFER:v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 100\r\n")
        offered = await viewer.recv()
        self.assertTrue(offered.startswith("OFFER:"))
        await caller.send("BYE")
        self.assertEqual(await viewer.recv(), "BYE")

    async def test_second_caller_is_busy_and_disconnect_frees_the_room(self) -> None:
        viewer = await open_client(self.port, "frame101")
        caller = await open_client(self.port, "frame101")
        await viewer.send("VIEW")
        await caller.send("HELLO:viewer")
        await viewer.recv()
        extra = await open_client(self.port, "frame101")
        await extra.send("HELLO:viewer")
        self.assertEqual(await extra.recv(), "486 Busy Here")
        caller.writer.close()
        await caller.writer.wait_closed()
        self.assertEqual(await viewer.recv(), "BYE")
        replacement_viewer = await open_client(self.port, "frame101")
        replacement_caller = await open_client(self.port, "frame101")
        await replacement_viewer.send("VIEW")
        await replacement_caller.send("HELLO:viewer")
        self.assertEqual(await replacement_viewer.recv(), "HELLO:viewer")

    async def test_cross_room_message_is_not_delivered(self) -> None:
        viewer_a = await open_client(self.port, "room-a")
        viewer_b = await open_client(self.port, "room-b")
        await viewer_a.send("VIEW")
        await viewer_b.send("VIEW")
        caller_a = await open_client(self.port, "room-a")
        await caller_a.send("HELLO:viewer")
        self.assertEqual(await viewer_a.recv(), "HELLO:viewer")
        waiter = asyncio.create_task(viewer_b.recv())
        done, _pending = await asyncio.wait({waiter}, timeout=0.2)
        self.assertFalse(done)
        waiter.cancel()

    async def test_production_idle_window_is_two_minutes(self) -> None:
        self.assertEqual(self._idle, 120.0)
        self.assertEqual(self._ping, 30.0)
        self.assertLess(self._ping, self._idle)

    async def test_silent_viewer_is_released_at_idle_timeout(self) -> None:
        import time

        viewer = await open_client(self.port, "frame101")
        await viewer.send("VIEW")
        peer = None
        for _ in range(50):
            state = frame_router.ROUTER.rooms.get("frame101")
            if state and state.get("viewer") is not None:
                peer = state["viewer"]
                break
            await asyncio.sleep(0.01)
        self.assertIsNotNone(peer)
        peer.seen = time.monotonic() - (frame_router.IDLE_TIMEOUT + 1)
        peer.last_tx_ping = peer.seen
        await frame_router.ROUTER.sweep()
        self.assertEqual(await viewer.recv(), "480 Temporarily Unavailable")

    async def test_pong_keeps_a_waiting_viewer(self) -> None:
        frame_router.IDLE_TIMEOUT = 0.45
        frame_router.PING_INTERVAL = 0.12
        viewer = await open_client(self.port, "frame101")
        await viewer.send("VIEW")
        await asyncio.sleep(0.15)
        await frame_router.ROUTER.sweep()
        opcode, payload = await viewer.recv_frame()
        self.assertEqual(opcode, 0x9)
        self.assertEqual(payload, b"")
        peer = frame_router.ROUTER.rooms["frame101"]["viewer"]
        seen = peer.seen
        await viewer.send_pong(payload)
        refreshed = False
        for _ in range(30):
            await asyncio.sleep(0.01)
            if peer.seen > seen:
                refreshed = True
                break
        self.assertTrue(refreshed)
        await asyncio.sleep(0.2)
        await frame_router.ROUTER.sweep()
        try:
            opcode, payload = await asyncio.wait_for(viewer.recv_frame(), timeout=0.15)
        except asyncio.TimeoutError:
            opcode, payload = 0x9, b""
        self.assertNotEqual(payload, b"480 Temporarily Unavailable")
        self.assertIn(opcode, (0x9, 0xA))

    async def test_caller_without_a_viewer_is_not_given_an_offer(self) -> None:
        caller = await open_client(self.port, "frame101")
        await caller.send("HELLO:viewer")
        waiter = asyncio.create_task(caller.recv())
        done, _pending = await asyncio.wait({waiter}, timeout=0.2)
        self.assertFalse(done)
        waiter.cancel()
        caller.writer.close()
        await caller.writer.wait_closed()

    async def test_log_records_the_verb_and_not_the_payload(self) -> None:
        import contextlib
        import io

        viewer = await open_client(self.port, "frame101")
        caller = await open_client(self.port, "frame101")
        await viewer.send("VIEW")
        await caller.send("HELLO:viewer")
        await viewer.recv()
        secret = "supersecretvalue"
        captured = io.StringIO()
        with contextlib.redirect_stdout(captured):
            await caller.send("ICE:0:candidate-" + secret)
            await viewer.recv()
        text = captured.getvalue()
        self.assertIn("kind=ICE", text)
        self.assertNotIn(secret, text)


if __name__ == "__main__":
    unittest.main()
