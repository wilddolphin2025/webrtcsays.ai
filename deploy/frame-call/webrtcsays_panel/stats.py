"""Turn directcall stdout into transport and media text.

Codec and resolution stay "not measured" until a stats object reports
a positive frame size. A running process is not a media state.
"""

import json


def redact(line):
    text = str(line).replace("\x00", "")
    lowered = text.lower()
    if (
        "bearer " in lowered
        or "token=" in lowered
        or "authorization:" in lowered
        or "credential" in lowered
    ):
        return "[redacted]"
    if len(text) > 240:
        return text[:240]
    return text


class MediaView:
    def __init__(self):
        self.have_stats = False
        self.ice = ""
        self.ice_connected_ms = 0
        self.video_codec = ""
        self.send_w = 0
        self.send_h = 0
        self.send_fps = 0.0
        self.recv_w = 0
        self.recv_h = 0
        self.recv_fps = 0.0
        self.frames_encoded = 0
        self.frames_decoded = 0
        self.audio_codec = ""
        self.audio_packets = 0
        self.remote_window = ""
        self.notes = []

    def ingest(self, line):
        clean = redact(line).strip()
        if not clean:
            return
        if clean.startswith("{") and clean.endswith("}"):
            try:
                obj = json.loads(clean)
            except json.JSONDecodeError:
                self._note(clean)
                return
            if isinstance(obj, dict) and obj.get("event") in {"stats", "summary"}:
                self._apply(obj)
                return
        if clean.startswith("remote window:"):
            self.remote_window = clean
        self._note(clean)

    def _note(self, clean):
        self.notes.append(clean)
        del self.notes[:-12]

    def _apply(self, obj):
        self.have_stats = True
        self.ice = str(obj.get("ice") or "")
        try:
            self.ice_connected_ms = int(obj.get("ice_connected_ms") or 0)
        except (TypeError, ValueError):
            self.ice_connected_ms = 0
        video = obj.get("video") if isinstance(obj.get("video"), dict) else {}
        audio = obj.get("audio") if isinstance(obj.get("audio"), dict) else {}
        self.video_codec = str(video.get("codec") or "")
        self.send_w = _pos_int(video.get("send_width"))
        self.send_h = _pos_int(video.get("send_height"))
        self.recv_w = _pos_int(video.get("recv_width"))
        self.recv_h = _pos_int(video.get("recv_height"))
        self.send_fps = _float(video.get("send_fps"))
        self.recv_fps = _float(video.get("recv_fps"))
        self.frames_encoded = _pos_int(video.get("frames_encoded"))
        self.frames_decoded = _pos_int(video.get("frames_decoded"))
        self.audio_codec = str(audio.get("codec") or "")
        self.audio_packets = _pos_int(audio.get("recv_packets")) + _pos_int(audio.get("send_packets"))

    def transport(self, process_running):
        if not process_running:
            return "not started"
        if not self.have_stats:
            return "process running, no stats yet"
        if self.ice_connected_ms > 0:
            return "ice connected " + (self.ice or "measured")
        return "stats, ice not connected"

    def send_text(self):
        if self.video_codec and self.send_w > 0 and self.send_h > 0:
            fps = " %.1f fps" % self.send_fps if self.send_fps else ""
            return "%s %dx%d%s" % (self.video_codec, self.send_w, self.send_h, fps)
        return "not measured"

    def recv_text(self):
        if self.video_codec and self.recv_w > 0 and self.recv_h > 0:
            fps = " %.1f fps" % self.recv_fps if self.recv_fps else ""
            return "%s %dx%d%s" % (self.video_codec, self.recv_w, self.recv_h, fps)
        return "not measured"

    def audio_text(self):
        if self.audio_packets > 0 and self.audio_codec:
            return self.audio_codec
        if self.audio_packets > 0:
            return "packets %d, codec not measured" % self.audio_packets
        return "not measured"


def _pos_int(value):
    try:
        number = int(value)
    except (TypeError, ValueError):
        return 0
    return number if number > 0 else 0


def _float(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return 0.0
