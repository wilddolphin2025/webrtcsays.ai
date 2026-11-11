"""Argv for the owned directcall. The token file is passed by path only."""

from pathlib import Path

SIGNAL_URL = "wss://rtc.wilddolphin.us:443/demo/view/ws/frame101"
CORES = (2, 3, 4, 7)
DEFAULT_DURATION_SEC = 900
MEMORY_LIMIT_BYTES = 7 * 1024 * 1024 * 1024
THERMAL_LIMIT_C = 80.0
THERMAL_SUSTAIN_SEC = 6.0


def directcall_argv(binary, image, token, duration_sec=DEFAULT_DURATION_SEC):
    binary = str(binary)
    image = str(image)
    token = str(token)
    duration = int(duration_sec)
    if duration < 1 or duration > 3600:
        raise ValueError("duration must be from 1 to 3600 seconds")
    return [
        binary,
        "--mode=caller",
        "--user_name=frame",
        "--target_name=viewer",
        "--room_name=frame101",
        "--encryption",
        "--signal-url=" + SIGNAL_URL,
        "--token-file=" + token,
        "--stun=stun:stun.l.google.com:19302",
        "--video-source=image",
        "--image-file=" + image,
        "--res=1920x1080",
        "--fps=30",
        "--remote-window=openvr",
        "--stats=5",
        "--duration=" + str(duration),
    ]


def default_paths(home):
    root = Path(home) / "babelfish" / "call" / "direct" / "pr21"
    return {
        "binary": root / "directcall",
        "image": root / "VR-Babelfish-1080p.png",
        "token": root / "token",
    }
