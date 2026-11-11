"""Copy the panel into the user directory and back up an existing desktop entry.

This does not call steamos-add-to-steam and does not touch other applications.
"""

import shutil
from pathlib import Path

DESKTOP_NAME = "WebRTCsays.AI.desktop"
COPY_NAMES = (
    "webrtcsays_app.sh",
    "webrtcsays_call.py",
    "WebRTCsays.AI.desktop",
    "README.md",
)


def install_tree(source, home, stamp):
    source = Path(source)
    home = Path(home)
    app = home / "babelfish" / "call" / "app"
    app.mkdir(parents=True, exist_ok=True)
    for name in COPY_NAMES:
        shutil.copy2(source / name, app / name)
    panel_dest = app / "webrtcsays_panel"
    if panel_dest.exists():
        shutil.rmtree(panel_dest)
    shutil.copytree(source / "webrtcsays_panel", panel_dest)
    applications = home / ".local" / "share" / "applications"
    applications.mkdir(parents=True, exist_ok=True)
    dest = applications / DESKTOP_NAME
    backup = None
    if dest.exists():
        backup = applications / ("WebRTCsays.AI.desktop.bak-" + stamp)
        backup.write_bytes(dest.read_bytes())
    shutil.copy2(source / DESKTOP_NAME, dest)
    return {"app": app, "desktop": dest, "backup": backup}
