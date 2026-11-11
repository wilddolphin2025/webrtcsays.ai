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
from webrtcsays_panel.compose import compose, read_remote_frame
from webrtcsays_panel.layout import HEIGHT, SURFACE_DISTANCE_METERS, SURFACE_WIDTH_METERS, WIDTH
from webrtcsays_panel.overlay_input import clicks_from_events
from webrtcsays_panel.owner import ProcFs
from webrtcsays_panel.present import UploadBudget
from webrtcsays_panel.session import Session
from webrtcsays_panel.command import default_paths

# Left of and closer than directcall's remote-video overlay at x=0, y=1.3, z=-2.
CONTROL_WIDTH_METERS = SURFACE_WIDTH_METERS


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
        "view: head-locked overlay, not eye fullscreen",
        "error: " + (session.error or "none"),
    ]
    rows.extend(session.lines()[-4:])
    return rows


def write_state(path, session):
    Path(path).write_text("\n".join(panel_lines(session)) + "\n", encoding="utf-8")


def handle_click(session, name):
    if name == "connect":
        return session.connect()
    if name == "hangup":
        return session.disconnect()
    return "ignored"


def make_session(home=None):
    home = Path.home() if home is None else Path(home)
    paths = default_paths(home)
    return Session(
        paths,
        ProcFs(),
        real_popen,
        os.sched_setaffinity,
        os.kill,
        time.time,
        time.sleep,
    )


def call_image(session):
    remote = read_remote_frame(_read_bytes(session.paths["frame"]))
    remote_pixels = remote_size = None
    if remote:
        remote_size = (remote[0], remote[1])
        remote_pixels = remote[3]
    return compose(panel_lines(session), remote_pixels, remote_size)


def snapshot_event(event):
    mouse = event.data.mouse
    return MouseSnapshot(event.eventType, float(mouse.x), float(mouse.y))


def _open_overlay():
    import openvr

    openvr.init(openvr.VRApplication_Overlay)
    overlay = openvr.IVROverlay()
    handle = overlay.createOverlay(OVERLAY_KEY, APP_NAME)
    overlay.setOverlayWidthInMeters(handle, SURFACE_WIDTH_METERS)
    if hasattr(overlay, "setOverlayCurvature"):
        overlay.setOverlayCurvature(handle, 0.35)
    overlay.setOverlayInputMethod(handle, openvr.VROverlayInputMethod_Mouse)
    # Laser hits outside the dashboard require this flag. Not verified on a headset.
    overlay.setOverlayFlag(handle, openvr.VROverlayFlags_MakeOverlaysInteractiveIfVisible, True)
    scale = openvr.HmdVector2_t()
    scale.v[0] = WIDTH
    scale.v[1] = HEIGHT
    overlay.setOverlayMouseScale(handle, scale)
    matrix = openvr.HmdMatrix34_t()
    matrix.m[0][0] = 1
    matrix.m[1][1] = 1
    matrix.m[2][2] = 1
    # Relative to the HMD: 0 is eye height, -Z is forward. Not a standing-world pose.
    matrix.m[0][3] = 0
    matrix.m[1][3] = 0
    matrix.m[2][3] = -SURFACE_DISTANCE_METERS
    overlay.setOverlayTransformTrackedDeviceRelative(
        handle, openvr.k_unTrackedDeviceIndex_Hmd, matrix
    )
    overlay.showOverlay(handle)
    return openvr, overlay, handle


_UPLOAD_BUF = {"buf": None}


def _upload(overlay, handle, pixels):
    # One ctypes buffer for every upload. A fresh array each 100 ms matched the shared RSS climb.
    import ctypes
    size = len(pixels)
    buf = _UPLOAD_BUF["buf"]
    if buf is None or len(buf) != size:
        buf = (c_uint8 * size)()
        _UPLOAD_BUF["buf"] = buf
    ctypes.memmove(buf, bytes(pixels), size)
    overlay.setOverlayRaw(handle, buf, WIDTH, HEIGHT, 4)


def _read_bytes(path):
    try:
        return Path(path).read_bytes()
    except OSError:
        return None


def _frame_index(path):
    import struct
    try:
        header = Path(path).read_bytes()[:16]
    except OSError:
        return None
    if len(header) < 16:
        return None
    magic, _width, _height, index = struct.unpack("<IIII", header)
    if magic != 0x314D5246:
        return None
    return index


def _thermal_zones(root="/sys/class/thermal"):
    texts = []
    base = Path(root)
    try:
        names = sorted(base.glob("thermal_zone*/temp"))
    except OSError:
        return texts
    for path in names:
        text = _read_optional(str(path))
        if text:
            texts.append(text)
    return texts


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
    session = make_session()
    state_path = Path.home() / "babelfish" / "call" / "app" / "state.txt"
    flag = {"stop": False}
    signal.signal(signal.SIGTERM, _request_stop(flag))
    signal.signal(signal.SIGINT, _request_stop(flag))
    vr = None
    try:
        vr = _open_overlay()
        openvr, overlay, handle = vr
        budget = UploadBudget()
        while not flag["stop"] and not session.stop_panel:
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
            zones = _thermal_zones()
            child_status = ""
            if session.identity is not None:
                child_status = _read_optional("/proc/%s/status" % session.identity.pid)
            self_status = _read_optional("/proc/self/status")
            session.poll(None, child_status, self_status, zones)
            signature = (tuple(panel_lines(session)), _frame_index(session.paths["frame"]))
            if budget.should_upload(signature, time.time()):
                _upload(overlay, handle, call_image(session))
                try:
                    state_path.parent.mkdir(parents=True, exist_ok=True)
                    write_state(state_path, session)
                except OSError:
                    pass
            time.sleep(0.05)
    finally:
        session.close()
        if vr is not None:
            try:
                vr[0].shutdown()
            except Exception:
                pass
