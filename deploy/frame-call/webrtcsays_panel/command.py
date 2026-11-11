"""Argv for the owned directcall. The token file is passed by path only."""

from pathlib import Path

SIGNAL_URL = "wss://rtc.wilddolphin.us:443/demo/view/ws/frame101"
CORES = (2, 3, 4, 7)
DEFAULT_DURATION_SEC = 900
MEMORY_LIMIT_BYTES = 7 * 1024 * 1024 * 1024
THERMAL_LIMIT_C = 80.0
THERMAL_SUSTAIN_SEC = 6.0


def directcall_argv(
    binary,
    image,
    token,
    frame_path,
    slot="1920x820",
    duration_sec=DEFAULT_DURATION_SEC,
    turns_file=None,
    ice_policy=None,
):
    binary = str(binary)
    image = str(image)
    token = str(token)
    duration = int(duration_sec)
    if duration < 1 or duration > 3600:
        raise ValueError("duration must be from 1 to 3600 seconds")
    if ice_policy not in (None, "", "all", "relay"):
        raise ValueError("ice policy must be all or relay")
    argv = [
        binary,
        "--mode=caller",
        "--user_name=frame",
        "--target_name=viewer",
        "--room_name=frame101",
        "--encryption",
        "--signal-url=" + SIGNAL_URL,
        "--token-file=" + token,
        "--stun=stun:stun.l.google.com:19302",
        "--video",
        "--video-source=image",
        "--image-file=" + image,
        "--res=1920x1080",
        "--fps=30",
        "--remote-window=app",
        "--remote-frame=" + str(frame_path),
        "--remote-slot=" + str(slot),
        "--stats=5",
        "--duration=" + str(duration),
    ]
    if turns_file:
        argv.append("--turns-file=" + str(turns_file))
    if ice_policy in ("all", "relay"):
        argv.append("--ice-policy=" + ice_policy)
    return argv


def resolve_direct_root(home, environ=None):
    """Versioned binary directory. Never defaults to a hardcoded pr21 path."""
    env = ""
    if environ is not None:
        env = str(environ.get("WEBRTCSAYS_DIRECT_ROOT", "")).strip()
    if env:
        return Path(env)
    path = Path(home) / "babelfish" / "call" / "app" / "direct.root"
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return None
    for raw in text.splitlines():
        line = raw.strip()
        if line and not line.startswith("#"):
            return Path(line)
    return None


def default_paths(home, root=None, environ=None):
    if root is None:
        root = resolve_direct_root(home, environ)
    frame = Path(home) / "babelfish" / "call" / "app" / "remote.frame"
    if root is None:
        return {"binary": None, "image": None, "token": None, "frame": frame, "root": None}
    root = Path(root)
    return {
        "binary": root / "directcall",
        "image": root / "VR-Babelfish-1080p.png",
        "token": root / "token",
        "frame": frame,
        "root": root,
    }
