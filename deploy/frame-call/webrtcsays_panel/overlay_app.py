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
from webrtcsays_panel.command import CORES, default_paths
from webrtcsays_panel.compose import read_remote_frame
from webrtcsays_panel.fast_compose import CallCanvas
from webrtcsays_panel.layout import HEIGHT, SURFACE_DISTANCE_METERS, SURFACE_WIDTH_METERS, WIDTH
from webrtcsays_panel.overlay_input import clicks_from_events
from webrtcsays_panel.owner import ProcFs
from webrtcsays_panel.present import UploadBudget
from webrtcsays_panel.session import Session
from webrtcsays_panel.turn import fetch_turn_account

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
        "turn: " + session.turn_note,
        "ice: " + (session.ice_policy or "all"),
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
    policy = os.environ.get("WEBRTCSAYS_ICE_POLICY", "").strip()
    if policy not in ("", "all", "relay"):
        policy = ""
    return Session(
        paths,
        ProcFs(),
        real_popen,
        os.sched_setaffinity,
        os.kill,
        time.time,
        time.sleep,
        turn_fetch=fetch_turn_account,
        ice_policy=policy or None,
    )


def frame_view(blob):
    parsed = read_remote_frame(blob)
    if not parsed:
        return None
    width, height, index, pixels = parsed
    if index % 2:
        return None
    return width, height, index, pixels


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
    # The ctypes buffer must be the full RGBA image. len() on the numpy
    # frame is only its height, and OpenVR then reads past that allocation.
    import ctypes
    from webrtcsays_panel.present import pixel_nbytes
    size = pixel_nbytes(pixels)
    buf = _UPLOAD_BUF["buf"]
    if buf is None or len(buf) != size:
        buf = (c_uint8 * size)()
        _UPLOAD_BUF["buf"] = buf
    source = pixels.ctypes.data if hasattr(pixels, "ctypes") else bytes(pixels)
    ctypes.memmove(buf, source, size)
    # One publish per new stable frame. The room shows through during a call
    # and not after Hangup, which is when this publish stops. That timing does
    # not show why the compositor clears the quad, so the call stays on this path.
    overlay.setOverlayRaw(handle, buf, WIDTH, HEIGHT, 4)


def overlay_lines(session):
    """Stable headset text. Log lines from HELLO retries are not part of the picture."""
    rows = [
        APP_NAME,
        "process: " + session.process_text,
    ]
    if session.error:
        rows[1] = "error: " + session.error
    return tuple(rows)


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
    if magic != 0x314D5246 or index % 2:
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


def release_overlay(vr):
    """Release the raw overlay while this process is still connected.

    Both compositor crashes were SIGBUS in a worker memcpy, 68 ms and 142 ms
    after the pipe dropped, at byte 0 of a read-only 1920x1080x4 tmpfs
    mapping. ClearOverlayTexture, hide, and destroy run before VR_Shutdown
    so that path is not the first time the compositor hears the client is
    gone. This does not prove the race is closed.
    """
    steps = []
    if not vr:
        return steps
    openvr_mod, overlay, handle = vr
    clear = getattr(overlay, "clearOverlayTexture", None)
    if clear is None:
        steps.append("clear-missing")
    else:
        try:
            clear(handle)
            steps.append("clear")
        except Exception:
            steps.append("clear-error")
    for name, fn in (
        ("hide", lambda: overlay.hideOverlay(handle)),
        ("destroy", lambda: overlay.destroyOverlay(handle)),
        ("shutdown", lambda: openvr_mod.shutdown()),
    ):
        try:
            fn()
            steps.append(name)
        except Exception:
            steps.append(name + "-error")
    return steps


def shutdown_runtime(vr, close_child):
    steps = release_overlay(vr)
    close_child()
    steps.append("child")
    return steps


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
        canvas = CallCanvas()
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
            image_lines = overlay_lines(session)
            index = _frame_index(session.paths["frame"])
            if budget.should_upload((image_lines, index), time.time()):
                canvas.set_status(image_lines)
                view = frame_view(_read_bytes(session.paths["frame"]))
                if view:
                    import numpy as np
                    width, height, _index, pixels = view
                    src = np.frombuffer(pixels, dtype=np.uint8).reshape(height, width, 4)
                    canvas.paint_video(src)
                _upload(overlay, handle, canvas.pixels)
            full = tuple(panel_lines(session))
            if full != flag.get("state"):
                flag["state"] = full
                try:
                    state_path.parent.mkdir(parents=True, exist_ok=True)
                    write_state(state_path, session)
                except OSError:
                    pass
            time.sleep(0.05)
    finally:
        # Release the compositor client before the child wait. The running
        # build called VR_Shutdown only after a 5s SIGTERM plus SIGKILL, with
        # the overlay still shown.
        shutdown_runtime(vr, session.close)
