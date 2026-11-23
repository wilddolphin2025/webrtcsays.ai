#!/usr/bin/env python3
"""Reference LiquidServer signaling (SIGNALING.md sections 1.1 and 2-4). No media.

This is an executable spec for the Go/Pion server, not a production server:
it speaks the direct text protocol as the callee, keeps room membership, and
builds a skeleton ANSWER that follows the codec and voice-swap rules. The
ANSWER has no ICE/DTLS lines, so a real browser cannot connect to it.

Run locally (loopback only):
    LIQUID_GATE_FILE=/path/to/gate python3 signaling_ref.py --port 7880
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import hashlib
import hmac
import json
import os
import re
import secrets
from dataclasses import dataclass, field
from pathlib import Path

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
ROOM_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$")
PATH_RE = re.compile(r"^/ws/([A-Za-z0-9][A-Za-z0-9_-]{0,63})$")
NAME_RE = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
GATE_RE = re.compile(r"^[0-9a-f]{64}$")
MAX_FRAME = 262144
MAX_PER_ROOM = 2
MAX_ROOMS = 8
IDLE_TIMEOUT = 120.0
VOICE_IDS = ("me", "female", "male")

OK = "200 OK"
BAD = "400 Bad Request"
UNAVAILABLE = "480 Temporarily Unavailable"
BUSY = "486 Busy Here"


# --------------------------------------------------------------------------- SDP rules

@dataclass
class AudioCodec:
    pt: int
    name: str
    clock: int
    channels: int
    fmtp: str = ""


@dataclass
class Media:
    kind: str
    mline: str
    lines: list[str] = field(default_factory=list)


def split_media(sdp: str) -> tuple[list[str], list[Media]]:
    session: list[str] = []
    media: list[Media] = []
    for line in [x for x in re.split(r"\r\n|\n", sdp) if x]:
        if line.startswith("m="):
            media.append(Media(kind=line[2:].split(" ", 1)[0], mline=line))
        elif media:
            media[-1].lines.append(line)
        else:
            session.append(line)
    return session, media


def audio_codecs(m: Media) -> list[AudioCodec]:
    order = [int(p) for p in m.mline.split(" ")[3:] if p.isdigit()]
    by_pt: dict[int, AudioCodec] = {}
    fmtp: dict[int, str] = {}
    for line in m.lines:
        r = re.match(r"^a=rtpmap:(\d+) ([^/\s]+)/(\d+)(?:/(\d+))?$", line)
        if r:
            pt = int(r.group(1))
            by_pt[pt] = AudioCodec(pt, r.group(2), int(r.group(3)), int(r.group(4) or 1))
        f = re.match(r"^a=fmtp:(\d+) (.*)$", line)
        if f:
            fmtp[int(f.group(1))] = f.group(2)
    out = []
    for pt in order:
        if pt in by_pt:
            c = by_pt[pt]
            c.fmtp = fmtp.get(pt, "")
            out.append(c)
    return out


def is_liquid(c: AudioCodec) -> bool:
    return c.name.upper() in ("LIQUIDCALL", "LIQUIDCODEC") and c.clock == 48000 and c.channels == 1


def is_opus(c: AudioCodec) -> bool:
    return c.name.lower() == "opus" and c.clock == 48000 and c.channels == 2


def requested_voice(m: Media, pt: int) -> str:
    for line in m.lines:
        r = re.match(rf"^a=x-liquidcall-voice:{pt} (\S+)$", line)
        if r:
            voice = r.group(1).lower()
            return voice if voice in VOICE_IDS else "me"
    return "me"


@dataclass
class Choice:
    kind: str | None            # "liquid", "opus" or None
    liquid_pt: int | None = None
    opus_pt: int | None = None
    voice: str = "me"           # active voice the server will apply


def choose(offer: str, loaded_voices: tuple[str, ...] = VOICE_IDS) -> Choice:
    _session, media = split_media(offer)
    audio = next((m for m in media if m.kind == "audio"), None)
    if audio is None:
        return Choice(None)
    codecs = audio_codecs(audio)
    liquid = next((c for c in codecs if is_liquid(c)), None)
    opus = next((c for c in codecs if is_opus(c)), None)
    if liquid is not None:
        want = requested_voice(audio, liquid.pt)
        active = want if want in loaded_voices else "me"
        return Choice("liquid", liquid.pt, opus.pt if opus else None, active)
    if opus is not None:
        return Choice("opus", None, opus.pt)
    return Choice(None)


def build_answer(offer: str, loaded_voices: tuple[str, ...] = VOICE_IDS) -> str | None:
    """Skeleton answer: codec lines and voice attributes only (no ICE/DTLS)."""
    choice = choose(offer, loaded_voices)
    if choice.kind is None:
        return None
    _session, media = split_media(offer)
    out = ["v=0", "o=- 0 2 IN IP4 127.0.0.1", "s=LiquidServer", "t=0 0"]
    for m in media:
        mid = next((l for l in m.lines if l.startswith("a=mid:")), None)
        if m.kind != "audio":
            parts = m.mline.split(" ")
            out.append(" ".join([parts[0], "0"] + parts[2:]))
            if mid:
                out.append(mid)
            continue
        pts: list[int] = []
        lines: list[str] = []
        if choice.kind == "liquid":
            pts.append(choice.liquid_pt)
            lines += [f"a=rtpmap:{choice.liquid_pt} LIQUIDCALL/48000/1",
                      f"a=fmtp:{choice.liquid_pt} pack=tight;fpp=1"]
            loaded = [v for v in VOICE_IDS if v in loaded_voices] or ["me"]
            lines += [f"a=x-liquidcall-voices:{choice.liquid_pt} {','.join(loaded)}",
                      f"a=x-liquidcall-voice:{choice.liquid_pt} {choice.voice}"]
        if choice.opus_pt is not None:
            pts.append(choice.opus_pt)
            lines += [f"a=rtpmap:{choice.opus_pt} opus/48000/2",
                      f"a=fmtp:{choice.opus_pt} minptime=10;useinbandfec=1"]
        parts = m.mline.split(" ")
        out.append(" ".join(parts[:3] + [str(p) for p in pts]))
        if mid:
            out.append(mid)
        out += ["a=sendrecv", "a=rtcp-mux", "a=extmap:1 urn:ietf:params:rtp-hdrext:sdes:mid"] + lines
    return "\r\n".join(out) + "\r\n"


# --------------------------------------------------------------------------- rooms / sessions

class Session:
    def __init__(self, room: str, writer: asyncio.StreamWriter) -> None:
        self.room = room
        self.writer = writer
        self.id = secrets.token_hex(4)
        self.state = "new"          # new -> hello -> invited -> answered
        self.name = ""
        self.choice: Choice | None = None
        self.closed = False
        self.sent: list[str] = []   # verbs only, for tests

    async def send(self, text: str) -> None:
        if self.closed:
            return
        data = text.encode("utf-8")
        head = bytearray([0x81])
        if len(data) < 126:
            head.append(len(data))
        elif len(data) <= 65535:
            head.append(126)
            head += len(data).to_bytes(2, "big")
        else:
            head.append(127)
            head += len(data).to_bytes(8, "big")
        self.sent.append(text.split(":", 1)[0])
        self.writer.write(bytes(head) + data)
        await self.writer.drain()

    async def close(self) -> None:
        if self.closed:
            return
        self.closed = True
        try:
            self.writer.write(b"\x88\x02\x03\xe8")
            await self.writer.drain()
            self.writer.close()
        except (ConnectionError, RuntimeError):
            pass


class Server:
    def __init__(self, gate: str, loaded_voices: tuple[str, ...] = VOICE_IDS) -> None:
        self.gate = gate
        self.loaded_voices = loaded_voices
        self.rooms: dict[str, list[Session]] = {}
        self.log: list[str] = []

    def note(self, event: str, s: Session, verb: str = "") -> None:
        # Verbs only: never SDP, candidates, names from the wire, or headers.
        self.log.append(f"{event} room={s.room} peer={s.id} {verb}".strip())

    def join(self, s: Session) -> str | None:
        members = self.rooms.get(s.room)
        if members is None:
            if len(self.rooms) >= MAX_ROOMS:
                return UNAVAILABLE
            members = self.rooms[s.room] = []
        if s in members:
            return None
        if len(members) >= MAX_PER_ROOM:
            return BUSY
        members.append(s)
        return None

    def leave(self, s: Session) -> None:
        members = self.rooms.get(s.room, [])
        if s in members:
            members.remove(s)
        if not members:
            self.rooms.pop(s.room, None)

    async def on_text(self, s: Session, text: str) -> bool:
        """Handle one message. Returns False when the session must close."""
        verb = text.split(":", 1)[0].strip()
        self.note("msg", s, verb if re.fullmatch(r"[A-Z_]{1,16}|\d{3} [A-Za-z ]{1,32}", verb) else "other")
        if text == "BYE" or text == "CANCEL":
            await s.send(OK)
            self.leave(s)
            return False
        if text == "HELLO" or text.startswith("HELLO:"):
            if s.state not in ("new", "hello"):
                return True                      # late retry; ignore
            name = text[6:] if text.startswith("HELLO:") else ""
            s.name = name if NAME_RE.fullmatch(name) else ""
            refusal = self.join(s)
            if refusal:
                await s.send(refusal)
                return False
            s.state = "hello"
            await s.send(OK)
            return True
        if text == "INVITE" or text.startswith("INVITE:"):
            if s.state != "hello":
                await s.send(BAD)
                return False
            if text.startswith("INVITE:"):
                try:
                    meta = json.loads(text[7:])
                except ValueError:
                    await s.send(BAD)
                    return False
                if not isinstance(meta, dict):
                    await s.send(BAD)
                    return False
                room_name = meta.get("room_name")
                if room_name is not None and room_name != s.room:
                    await s.send(BAD)
                    return False
            s.state = "invited"
            await s.send("WAITING")
            return True
        if text.startswith("OFFER:"):
            if s.state not in ("invited", "answered"):
                await s.send(BAD)
                return False
            answer = build_answer(text[6:], self.loaded_voices)
            if answer is None:
                await s.send(BAD)
                return False
            s.choice = choose(text[6:], self.loaded_voices)
            s.state = "answered"
            await s.send("ANSWER:" + answer)
            return True
        if text.startswith("ICE:"):
            parts = text.split(":", 2)
            if len(parts) != 3 or not parts[1].isdigit() or not parts[2].startswith("candidate:"):
                return True                      # malformed candidate: drop, keep the call
            return True
        if text == OK:
            return True                          # ack of our BYE
        return True                              # unknown verbs are ignored


def accept_key(key: str) -> str:
    return base64.b64encode(hashlib.sha1((key + GUID).encode("ascii")).digest()).decode("ascii")


async def reject(writer: asyncio.StreamWriter, status: int, reason: str) -> None:
    writer.write(f"HTTP/1.1 {status} {reason}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".encode())
    await writer.drain()
    writer.close()


async def read_frame(reader: asyncio.StreamReader) -> tuple[int, bytes] | None:
    head = await asyncio.wait_for(reader.readexactly(2), timeout=IDLE_TIMEOUT)
    opcode, masked, length = head[0] & 0x0F, bool(head[1] & 0x80), head[1] & 0x7F
    if length == 126:
        length = int.from_bytes(await reader.readexactly(2), "big")
    elif length == 127:
        length = int.from_bytes(await reader.readexactly(8), "big")
    if length > MAX_FRAME or not masked:
        return None                              # clients MUST mask
    mask = await reader.readexactly(4)
    payload = await reader.readexactly(length)
    return opcode, bytes(b ^ mask[i % 4] for i, b in enumerate(payload))


def make_handler(server: Server):
    async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            raw = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout=5)
        except (asyncio.TimeoutError, asyncio.IncompleteReadError, asyncio.LimitOverrunError):
            writer.close()
            return
        lines = raw.decode("latin1").split("\r\n")
        try:
            method, target, _version = lines[0].split(" ", 2)
        except ValueError:
            await reject(writer, 400, "Bad Request")
            return
        headers = {}
        for line in lines[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                headers[k.strip().lower()] = v.strip()
        gate = headers.get("x-liquid-gate", "")
        if not (len(gate) == len(server.gate) and hmac.compare_digest(gate, server.gate)):
            await reject(writer, 401, "Unauthorized")
            return
        if "?" in target:
            await reject(writer, 400, "Bad Request")
            return
        if method == "GET" and target == "/healthz" and "sec-websocket-key" not in headers:
            writer.write(b"HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: close\r\n\r\nok\n")
            await writer.drain()
            writer.close()
            return
        match = PATH_RE.fullmatch(target)
        key = headers.get("sec-websocket-key", "")
        if method != "GET" or not match or match.group(1) == "ws":
            await reject(writer, 404, "Not Found")
            return
        if not key or headers.get("upgrade", "").lower() != "websocket":
            await reject(writer, 400, "Bad Request")
            return
        writer.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      f"Sec-WebSocket-Accept: {accept_key(key)}\r\n\r\n").encode())
        await writer.drain()
        s = Session(match.group(1), writer)
        server.note("open", s)
        try:
            while not s.closed:
                frame = await read_frame(reader)
                if frame is None:
                    break
                opcode, payload = frame
                if opcode == 0x8:
                    break
                if opcode == 0x9:
                    writer.write(bytes([0x8A, len(payload)]) + payload)
                    await writer.drain()
                    continue
                if opcode != 0x1:
                    continue
                if not await server.on_text(s, payload.decode("utf-8", "replace")):
                    break
        except (asyncio.TimeoutError, asyncio.IncompleteReadError, ConnectionError):
            pass
        finally:
            server.leave(s)
            server.note("close", s)
            await s.close()

    return handle


def load_gate(path: str) -> str:
    value = Path(path).read_text(encoding="ascii").strip()
    if not GATE_RE.fullmatch(value):
        raise SystemExit("gate file must hold 64 lowercase hex characters")
    return value


async def serve(server: Server, host: str, port: int) -> asyncio.base_events.Server:
    if host not in ("127.0.0.1", "::1"):
        raise SystemExit("refusing non-loopback bind")
    return await asyncio.start_server(make_handler(server), host, port)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7880)
    args = ap.parse_args()
    gate = load_gate(os.environ.get("LIQUID_GATE_FILE", "/etc/liquidserver/gate"))

    async def run() -> None:
        srv = await serve(Server(gate), args.bind, args.port)
        async with srv:
            await srv.serve_forever()

    asyncio.run(run())


if __name__ == "__main__":
    main()
