#!/usr/bin/env python3
"""Local-only preview. Binds 127.0.0.1 and adds the router auth header.

Not for the VPS. Production uses nginx auth_request and the router alone.
"""

from __future__ import annotations

import argparse
import asyncio
import pathlib

PAGE = pathlib.Path(__file__).resolve().parents[1] / "page"
AUTH = b"X-Demo-Auth: ok\r\n"


async def pipe(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    try:
        while not reader.at_eof():
            data = await reader.read(65536)
            if not data:
                break
            writer.write(data)
            await writer.drain()
    except ConnectionError:
        pass
    finally:
        try:
            writer.close()
        except ConnectionError:
            pass


async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter, router_port: int) -> None:
    header = b""
    while b"\r\n\r\n" not in header:
        chunk = await reader.read(4096)
        if not chunk:
            writer.close()
            return
        header += chunk
    request, _, rest = header.partition(b"\r\n\r\n")
    line = request.split(b"\r\n", 1)[0].decode("latin1")
    parts = line.split(" ")
    path = parts[1].split("?", 1)[0] if len(parts) > 1 else "/"
    if path.startswith("/demo/view/ws/"):
        upstream_reader, upstream_writer = await asyncio.open_connection("127.0.0.1", router_port)
        upstream_writer.write(request + b"\r\n" + AUTH + b"\r\n")
        await upstream_writer.drain()
        await asyncio.gather(pipe(reader, upstream_writer), pipe(upstream_reader, writer))
        return
    if path.startswith("/demo/view/assets/"):
        name = path.rsplit("/", 1)[-1]
        target = PAGE / name
        body = target.read_bytes() if target.is_file() else b""
        content_type = "text/css" if name.endswith(".css") else "application/javascript"
    elif path.startswith("/demo/view/"):
        body = (PAGE / "index.html").read_bytes()
        content_type = "text/html"
    elif path == "/local-caller/":
        body = (pathlib.Path(__file__).resolve().parent / "local_caller.html").read_bytes()
        content_type = "text/html"
    elif path == "/demo/turn":
        body = b'{"error":"unavailable"}'
        content_type = "application/json"
        writer.write(
            f"HTTP/1.1 503 Unavailable\r\nContent-Type: {content_type}\r\n"
            f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n".encode() + body
        )
        await writer.drain()
        writer.close()
        return
    else:
        body = b"not found"
        content_type = "text/plain"
        writer.write(
            f"HTTP/1.1 404 Not Found\r\nContent-Type: {content_type}\r\n"
            f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n".encode() + body
        )
        await writer.drain()
        writer.close()
        return
    writer.write(
        f"HTTP/1.1 200 OK\r\nContent-Type: {content_type}\r\nContent-Length: {len(body)}\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n".encode() + body
    )
    await writer.drain()
    writer.close()


async def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--router-port", type=int, default=3461)
    args = parser.parse_args()

    async def connection(reader, writer):
        await handle(reader, writer, args.router_port)

    server = await asyncio.start_server(connection, "127.0.0.1", args.port)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
