"""Headset panel. The picture is an OpenVR overlay, not a desktop-only window."""

import os
import queue
import signal
import subprocess
import threading
import time
from ctypes import c_uint8
from pathlib import Path

from webrtcsays_panel import APP_NAME, OVERLAY_KEY
from webrtcsays_panel.command import CORES
from webrtcsays_panel.layout import BUTTONS, HEIGHT, WIDTH
from webrtcsays_panel.overlay_input import clicks_from_events
from webrtcsays_panel.owner import ProcFs
from webrtcsays_panel.session import Session
from webrtcsays_panel.command import default_paths

# Left of and closer than directcall's remote-video overlay at x=0, y=1.3, z=-2.
CONTROL_WIDTH_METERS = 1.1


class _Mouse:
    def __init__(self, x, y):
        self.x = x
        self.y = y


class _Data:
    def __init__(self, x, y):
        self.mouse = _Mouse(x, y)


class MouseSnapshot:
    def __init__(self, event_type, x, y):
        self.eventType = event_type
        self.data = _Data(x, y)


class PipeChild:
    def __init__(self, popen):
        self.pid = popen.pid
        self._queue = queue.Queue()

        def read():
            for line in popen.stdout:
                self._queue.put(line.decode("utf-8", "replace"))

        threading.Thread(target=read, daemon=True).start()

    def drain(self):
        lines = []
        while True:
            try:
                lines.append(self._queue.get_nowait())
            except queue.Empty:
                return lines


def real_popen(argv):
    proc = subprocess.Popen(
        argv,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    return PipeChild(proc)


def panel_lines(session):
    media = session.media
    rows = [
        APP_NAME,
        "process: " + session.process_text,
        "transport: " + media.transport(session.process_text == "running"),
        "send: " + media.send_text(),
        "receive: " + media.recv_text(),
        "audio: " + media.audio_text(),
        "remote window: " + (media.remote_window or "not reported"),
        "session limit: %ds on cores %s" % (session.duration_sec, ",".join(str(c) for c in CORES)),
        "error: " + (session.error or "none"),
    ]
    rows.extend(session.lines()[-4:])
    return rows


def write_state(path, session):
    Path(path).write_text("\n".join(panel_lines(session)) + "\n", encoding="utf-8")


def handle_click(session, name):
    if name == "connect":
        return session.connect()
    if name == "disconnect":
        return session.disconnect()
    return "ignored"


def make_session(home=None):
    paths = default_paths(Path.home() if home is None else home)
    return Session(
        paths,
        ProcFs(),
        real_popen,
        os.sched_setaffinity,
        os.kill,
        time.time,
        time.sleep,
    )


def draw(surface, title_font, body_font, lines):
    import pygame

    surface.fill((27, 40, 56))
    surface.blit(title_font.render(lines[0], True, (255, 255, 255)), (48, 36))
    for name, (x, y, w, h) in BUTTONS.items():
        color = (26, 159, 255) if name == "connect" else (42, 71, 94)
        pygame.draw.rect(surface, color, pygame.Rect(x, y, w, h), border_radius=12)
        label = "Connect" if name == "connect" else "Disconnect"
        ink = (14, 20, 27) if name == "connect" else (199, 213, 224)
        surface.blit(title_font.render(label, True, ink), (x + 36, y + 48))
    y = 460
    for line in lines[1:]:
        surface.blit(body_font.render(line, True, (199, 213, 224)), (48, y))
        y += 32
        if y > HEIGHT - 24:
            break


def snapshot_event(event):
    mouse = event.data.mouse
    return MouseSnapshot(event.eventType, float(mouse.x), float(mouse.y))


def _open_overlay():
    import openvr

    openvr.init(openvr.VRApplication_Overlay)
    overlay = openvr.IVROverlay()
    handle = overlay.createOverlay(OVERLAY_KEY, APP_NAME)
    overlay.setOverlayWidthInMeters(handle, CONTROL_WIDTH_METERS)
    overlay.setOverlayInputMethod(handle, openvr.VROverlayInputMethod_Mouse)
    scale = openvr.HmdVector2_t()
    scale.v[0] = WIDTH
    scale.v[1] = HEIGHT
    overlay.setOverlayMouseScale(handle, scale)
    matrix = openvr.HmdMatrix34_t()
    matrix.m[0][0] = 1
    matrix.m[1][1] = 1
    matrix.m[2][2] = 1
    matrix.m[0][3] = -0.95
    matrix.m[1][3] = 0.85
    matrix.m[2][3] = -1.6
    overlay.setOverlayTransformAbsolute(handle, openvr.TrackingUniverseStanding, matrix)
    overlay.showOverlay(handle)
    return openvr, overlay, handle


def _upload(overlay, handle, surface):
    import pygame

    raw = pygame.image.tobytes(surface, "RGBA")
    buf = (c_uint8 * len(raw)).from_buffer_copy(raw)
    overlay.setOverlayRaw(handle, buf, WIDTH, HEIGHT, 4)


def _read_optional(path):
    try:
        return Path(path).read_text(encoding="utf-8")
    except OSError:
        return None


def _request_stop(flag):
    def handler(signum, frame):
        del signum, frame
        flag["stop"] = True

    return handler


def main():
    os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
    os.environ.setdefault("SDL_AUDIODRIVER", "dummy")
    import pygame

    pygame.font.init()
    surface = pygame.Surface((WIDTH, HEIGHT))
    title_font = pygame.font.Font(None, 72)
    body_font = pygame.font.Font(None, 36)
    session = make_session()
    state_path = Path.home() / "babelfish" / "call" / "app" / "state.txt"
    flag = {"stop": False}
    signal.signal(signal.SIGTERM, _request_stop(flag))
    signal.signal(signal.SIGINT, _request_stop(flag))
    vr = None
    try:
        vr = _open_overlay()
        openvr, overlay, handle = vr
        while not flag["stop"]:
            events = []
            current = openvr.VREvent_t()
            for _ in range(32):
                ok, current = overlay.pollNextOverlayEvent(handle, current)
                if not ok:
                    break
                events.append(snapshot_event(current))
                current = openvr.VREvent_t()
            for name in clicks_from_events(events):
                handle_click(session, name)
            thermal = _read_optional("/sys/class/thermal/thermal_zone0/temp")
            status = ""
            if session.identity is not None:
                status = _read_optional("/proc/%s/status" % session.identity.pid)
            lines = panel_lines(session)
            session.poll(thermal, status)
            lines = panel_lines(session)
            draw(surface, title_font, body_font, lines)
            _upload(overlay, handle, surface)
            try:
                state_path.parent.mkdir(parents=True, exist_ok=True)
                write_state(state_path, session)
            except OSError:
                pass
            time.sleep(0.1)
    finally:
        session.close()
        if vr is not None:
            try:
                vr[0].shutdown()
            except Exception:
                pass
