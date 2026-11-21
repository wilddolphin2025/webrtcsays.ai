#!/usr/bin/env python3
"""Checks that the WebSocket bridge ends the call when a browser vanishes.

Starts a call over ws:// (HELLO -> 200 OK -> INVITE -> WAITING), then drops
the TCP connection without a close frame, as a crashed tab would. The callee
must log the hangup instead of staying busy.
Usage: test_ws_bridge.py <directcall> <cert> <key>
"""
import base64, os, socket, struct, subprocess, sys, time

BIN, CERT, KEY = sys.argv[1:4]
TCP_PORT, WS_PORT = 3476, 3477
LOG = "/tmp/direct_ws_bridge.log"


def ws_connect(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
               f"Sec-WebSocket-Version: 13\r\n\r\n").encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        resp += s.recv(4096)
    assert b" 101 " in resp, resp
    return s


def ws_send(s, text):
    data = text.encode()
    mask = os.urandom(4)
    hdr = bytes([0x81, 0x80 | len(data)]) if len(data) < 126 else \
        bytes([0x81, 0x80 | 126]) + struct.pack(">H", len(data))
    s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))


def ws_recv_text(s):
    while True:
        b0, b1 = s.recv(2)
        n = b1 & 0x7F
        if n == 126:
            n = struct.unpack(">H", s.recv(2))[0]
        payload = b""
        while len(payload) < n:
            payload += s.recv(n - len(payload))
        if b0 & 0x0F == 0x1:
            return payload.decode()


def wait_log(pattern, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if pattern in open(LOG, errors="replace").read():
            return True
        time.sleep(0.1)
    return False


env = dict(os.environ, DIRECT_LOG_LEVEL="info")
callee = subprocess.Popen(
    [BIN, "--mode=callee", "--user_name=robot", "--encryption",
     f"--webrtc_cert_path={CERT}", f"--webrtc_key_path={KEY}",
     "--no-whisper", "--no-llama", "--no-video", "--websocket_signaling",
     f"--websocket_port={WS_PORT}", f"127.0.0.1:{TCP_PORT}"],
    stdout=open(LOG, "w"), stderr=subprocess.STDOUT, env=env)
try:
    assert wait_log("WebSocket signaling listening", 10), "bridge did not start"
    ws = ws_connect(WS_PORT)
    ws_send(ws, "HELLO:robot")
    assert ws_recv_text(ws) == "200 OK"
    ws_send(ws, 'INVITE:{"agent":"audio","video":false}')
    assert ws_recv_text(ws) == "WAITING"
    # Vanish: RST, no close frame.
    ws.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    ws.close()
    assert wait_log("Ending call (remote BYE)", 5), "callee stayed busy"
    print("PASS ws: browser vanished -> bridge sent BYE -> callee hung up")

    # The callee must be free for the next browser.
    ws = ws_connect(WS_PORT)
    ws_send(ws, "HELLO:robot")
    assert ws_recv_text(ws) == "200 OK", "callee still busy"
    print("PASS ws: next browser accepted")
    ws.close()
finally:
    callee.terminate()
    callee.wait(timeout=15)
