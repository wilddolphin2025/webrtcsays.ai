"""Copy the call app into ~/babelfish/call/app only.

Does not install a desktop entry, autostart file, or any launcher registration.
"""

import shutil
from pathlib import Path

COPY_NAMES = (
    "webrtcsays_app.sh",
    "webrtcsays_call.py",
    "README.md",
)


def install_tree(source, home, stamp):
    del stamp
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
    return {"app": app}
