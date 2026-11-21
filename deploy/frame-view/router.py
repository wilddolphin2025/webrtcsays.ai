#!/usr/bin/env python3
"""Loopback room router for the Frame viewer.

Pairs one native caller and one browser viewer in a single room and forwards
the existing direct text protocol. It does not talk to the Slim callee.
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import hashlib
import hmac
import os
import re
import secrets
import time

ROOM_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$")
PATH_RE = re.compile(r"^/demo/view/ws/([A-Za-z0-9][A-Za-z0-9_-]{0,63})$")
AUTH_HEADER = "x-demo-auth"
AUTH_VALUE = "ok"
MAX_ROOMS = 32
MAX_CLIENTS = 64
MAX_QUEUED = 32
MAX_QUEUE_BYTES = 1024 * 1024
MAX_FRAME = 262144
SETUP_TIMEOUT = 45.0
IDLE_TIMEOUT = 120.0
QUEUE_TIMEOUT = 20.0
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def log(event: str, **fields: str) -> None:
    parts = [event]
    for key in sorted(fields):
        value = fields[key]
        allowed = r"[A-Za-z0-9 ]{1,24}" if key == "kind" else r"[A-Za-z0-9_.:-]{0,80}"
        if not re.fullmatch(allowed, value or ""):
            value = "redacted"
        parts.append(f"{key}={value}")
    print(" ".join(parts), flush=True)


def verb(message: str) -> str:
    head = message.split(":", 1)[0].split("\r", 1)[0].split("\n", 1)[0]
    if not re.fullmatch(r"[A-Za-z0-9 ]{1,24}", head):
        return "binary"
    return head.strip() or "empty"


class Peer:
    def __init__(self, room: str, peer_id: str, writer: asyncio.StreamWriter) -> None:
        self.room = room
        self.peer_id = peer_id
        self.writer = writer
        self.role: str | None = None
        self.queue: list[tuple[float, str]] = []
        self.queued_bytes = 0
        self.opened = time.monotonic()
        self.seen = self.opened
        self.hello_at: float | None = None
        self.answer_at: float | None = None
        self.closed = False

    async def send(self, text: str) -> bool:
        if self.closed:
            return False
        payload = text.encode("utf-8")
        header = bytearray([0x81])
        length = len(payload)
        if length < 126:
            header.append(length)
        elif length <= 65535:
            header.append(126)
            header += length.to_bytes(2, "big")
        else:
            return False
        try:
            self.writer.write(header + payload)
            await self.writer.drain()
        except (ConnectionError, asyncio.IncompleteReadError):
            return False
        return True

    async def close(self) -> None:
        if self.closed:
            return
        self.closed = True
        try:
            self.writer.write(bytes([0x88, 0x00]))
            await self.writer.drain()
        except ConnectionError:
            pass
        try:
            self.writer.close()
            await self.writer.wait_closed()
        except ConnectionError:
            pass


class Router:
    def __init__(self) -> None:
        self.rooms: dict[str, dict[str, Peer | None]] = {}
        self.clients = 0

    def room_state(self, room: str) -> dict[str, Peer | None]:
        state = self.rooms.get(room)
        if state is None:
            if len(self.rooms) >= MAX_ROOMS:
                raise RuntimeError("room-cap")
            state = {"caller": None, "viewer": None}
            self.rooms[room] = state
        return state

    def drop_room_if_empty(self, room: str) -> None:
        state = self.rooms.get(room)
        if state and state["caller"] is None and state["viewer"] is None:
            self.rooms.pop(room, None)

    async def claim(self, peer: Peer, role: str) -> bool:
        try:
            state = self.room_state(peer.room)
        except RuntimeError:
            await peer.send("480 Temporarily Unavailable")
            return False
        current = state[role]
        if current is not None and current is not peer:
            await peer.send("486 Busy Here" if role == "caller" else "480 Temporarily Unavailable")
            log("busy", room=peer.room, peer=peer.peer_id, role=role)
            return False
        state[role] = peer
        peer.role = role
        log("claim", room=peer.room, peer=peer.peer_id, role=role)
        partner = state["viewer" if role == "caller" else "caller"]
        if partner is not None:
            queued = list(partner.queue)
            partner.queue.clear()
            partner.queued_bytes = 0
            for _, text in queued:
                await peer.send(text)
        return True

    def partner(self, peer: Peer) -> Peer | None:
        state = self.rooms.get(peer.room)
        if not state or not peer.role:
            return None
        other = state["viewer" if peer.role == "caller" else "caller"]
        if other is peer:
            return None
        return other

    async def release(self, peer: Peer, notify: bool) -> None:
        state = self.rooms.get(peer.room)
        if state and peer.role and state.get(peer.role) is peer:
            other = self.partner(peer)
            state[peer.role] = None
            if notify and other is not None and not other.closed:
                await other.send("BYE")
                await other.close()
                if other.role and state.get(other.role) is other:
                    state[other.role] = None
        self.drop_room_if_empty(peer.room)
        await peer.close()
        log("release", room=peer.room, peer=peer.peer_id, role=peer.role or "none")

    async def forward(self, peer: Peer, text: str) -> bool:
        other = self.partner(peer)
        if other is None:
            if len(peer.queue) >= MAX_QUEUED or peer.queued_bytes + len(text) > MAX_QUEUE_BYTES:
                await peer.send("480 Temporarily Unavailable")
                return False
            peer.queue.append((time.monotonic(), text))
            peer.queued_bytes += len(text)
            return True
        return await other.send(text)

    async def sweep(self) -> None:
        now = time.monotonic()
        doomed: list[Peer] = []
        for state in self.rooms.values():
            for peer in (state["caller"], state["viewer"]):
                if peer is None or peer.closed:
                    continue
                if now - peer.seen > IDLE_TIMEOUT:
                    doomed.append(peer)
                    continue
                if peer.queue and now - peer.queue[0][0] > QUEUE_TIMEOUT:
                    doomed.append(peer)
                    continue
                if peer.role == "caller" and peer.hello_at and peer.answer_at is None:
                    if now - peer.hello_at > SETUP_TIMEOUT:
                        doomed.append(peer)
        for peer in doomed:
            await peer.send("480 Temporarily Unavailable")
            await self.release(peer, notify=True)


ROUTER = Router()


async def read_headers(reader: asyncio.StreamReader) -> tuple[str, dict[str, str]] | None:
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = await asyncio.wait_for(reader.read(4096), timeout=5)
        if not chunk:
            return None
        data += chunk
        if len(data) > 16384:
            return None
    head, _sep, _rest = data.partition(b"\r\n\r\n")
    lines = head.decode("latin1", "replace").split("\r\n")
    if not lines or " " not in lines[0]:
        return None
    target = lines[0].split(" ")[1]
    headers: dict[str, str] = {}
    for line in lines[1:]:
        if ":" not in line:
            continue
        name, value = line.split(":", 1)
        headers[name.strip().lower()] = value.strip()
    return target, headers


def accept_key(secret: str) -> str:
    digest = hashlib.sha1((secret + GUID).encode("ascii")).digest()
    return base64.b64encode(digest).decode("ascii")


async def reject(writer: asyncio.StreamWriter, status: int, reason: str) -> None:
    body = reason.encode("ascii", "replace")
    writer.write(
        f"HTTP/1.1 {status} {reason}\r\nContent-Length: {len(body)}\r\n"
        "Connection: close\r\n\r\n".encode("ascii")
        + body
    )
    await writer.drain()
    writer.close()


async def read_frame(reader: asyncio.StreamReader) -> tuple[int, bytes] | None:
    try:
        header = await asyncio.wait_for(reader.readexactly(2), timeout=IDLE_TIMEOUT)
    except (asyncio.TimeoutError, asyncio.IncompleteReadError):
        return None
    opcode = header[0] & 0x0F
    masked = (header[1] & 0x80) != 0
    length = header[1] & 0x7F
    if length == 126:
        raw = await reader.readexactly(2)
        length = int.from_bytes(raw, "big")
    elif length == 127:
        return None
    if length > MAX_FRAME:
        return None
    mask = await reader.readexactly(4) if masked else b""
    payload = await reader.readexactly(length) if length else b""
    if masked:
        payload = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
    return opcode, payload


async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    if ROUTER.clients >= MAX_CLIENTS:
        await reject(writer, 503, "Busy")
        return
    try:
        opened = await read_headers(reader)
    except asyncio.TimeoutError:
        await reject(writer, 408, "Timeout")
        return
    if opened is None:
        await reject(writer, 400, "Bad Request")
        return
    target, headers = opened
    path = target.split("?", 1)[0]
    presented = headers.get(AUTH_HEADER, "")
    if "?" in target or len(presented) != len(AUTH_VALUE) or not hmac.compare_digest(presented, AUTH_VALUE):
        await reject(writer, 401, "Unauthorized")
        log("reject", reason="auth")
        return
    match = PATH_RE.fullmatch(path)
    if not match or not ROOM_RE.fullmatch(match.group(1)):
        await reject(writer, 404, "Not Found")
        return
    key = headers.get("sec-websocket-key", "")
    if not key:
        await reject(writer, 400, "Bad Request")
        return
    room = match.group(1)
    peer_id = secrets.token_hex(4)
    writer.write(
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {accept_key(key)}\r\n\r\n".encode("ascii")
    )
    await writer.drain()
    peer = Peer(room, peer_id, writer)
    ROUTER.clients += 1
    log("open", room=room, peer=peer_id, role="pending")
    try:
        while not peer.closed:
            frame = await read_frame(reader)
            if frame is None:
                break
            opcode, payload = frame
            peer.seen = time.monotonic()
            if opcode == 0x8:
                break
            if opcode == 0x9:
                writer.write(bytes([0x8A, len(payload)]) + payload)
                await writer.drain()
                continue
            if opcode != 0x1:
                continue
            text = payload.decode("utf-8", "replace")
            kind = verb(text)
            log("msg", room=room, peer=peer_id, role=peer.role or "pending", kind=kind)
            if peer.role is None:
                if kind == "VIEW":
                    if not await ROUTER.claim(peer, "viewer"):
                        break
                    continue
                if kind == "HELLO":
                    if not await ROUTER.claim(peer, "caller"):
                        break
                    peer.hello_at = time.monotonic()
                else:
                    await peer.send("400 Bad Request")
                    break
            if kind == "ANSWER" and peer.role == "viewer":
                caller = ROUTER.partner(peer)
                if caller is not None:
                    caller.answer_at = time.monotonic()
            if kind == "BYE":
                await ROUTER.forward(peer, text)
                await ROUTER.release(peer, notify=False)
                break
            if not await ROUTER.forward(peer, text):
                break
    except (asyncio.IncompleteReadError, ConnectionError, asyncio.TimeoutError):
        pass
    finally:
        ROUTER.clients = max(0, ROUTER.clients - 1)
        if not peer.closed:
            await ROUTER.release(peer, notify=True)


async def sweeper() -> None:
    while True:
        await asyncio.sleep(1)
        await ROUTER.sweep()


def constant_auth(presented: str) -> bool:
    return hmac.compare_digest(presented, AUTH_VALUE)


async def serve(host: str, port: int, sweep: bool = False) -> asyncio.Server:
    if host not in {"127.0.0.1", "::1"}:
        raise SystemExit("refusing non-loopback bind")
    server = await asyncio.start_server(handle, host, port)
    if sweep:
        asyncio.create_task(sweeper())
    sockets = ", ".join(str(sock.getsockname()) for sock in server.sockets or [])
    log("listen", host=host, port=str(port), sockets=sockets.replace(" ", ""))
    return server


def main() -> None:
    parser = argparse.ArgumentParser(description="Frame viewer room router")
    parser.add_argument("--bind", default=os.environ.get("VIEW_ROUTER_BIND", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("VIEW_ROUTER_PORT", "3461")))
    args = parser.parse_args()
    if not constant_auth(AUTH_VALUE):
        raise SystemExit("auth check failed")

    async def run() -> None:
        server = await serve(args.bind, args.port, sweep=True)
        async with server:
            await server.serve_forever()

    asyncio.run(run())


if __name__ == "__main__":
    main()
