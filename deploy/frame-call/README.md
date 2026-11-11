# WebRTCsays.AI Frame call panel

A headset control panel for the directcall sender. Connect and Disconnect are drawn on an OpenVR overlay named WebRTCsays.AI. The remote picture stays on directcall's separate overlay, `webrtcsays.remote`. This panel uses `webrtcsays.call.controls`.

The panel is pygame drawing uploaded with the pyopenvr already installed in `~/babelfish/venv`. It does not use Pillow, Tk, or Qt. A desktop window is not the control surface. SDL audio is left on the dummy driver so this process does not take the microphone or the speaker. directcall owns playback and the mic.

Nothing here was installed on a Frame or a VPS, and `steamos-add-to-steam` was not run. A person in the headset has not clicked these controls. The tests mock process ownership and the overlay mouse mapping.

## Install

From a checkout of this directory, on the Frame, as the normal user:

```sh
python3 - << 'PY'
from pathlib import Path
from webrtcsays_panel.install import install_tree
install_tree(Path(".").resolve(), Path.home(), "manual")
PY
chmod 755 "$HOME/babelfish/call/app/webrtcsays_app.sh"
```

That copies the app to `~/babelfish/call/app` and writes `~/.local/share/applications/WebRTCsays.AI.desktop`. If that desktop file already exists, the previous bytes are kept as `WebRTCsays.AI.desktop.bak-manual`. Other desktop files are not modified.

The call files are expected to already be in place:

- `~/babelfish/call/direct/pr21/directcall`
- `~/babelfish/call/direct/pr21/VR-Babelfish-1080p.png`
- `~/babelfish/call/direct/pr21/token`

The token file is passed with `--token-file`. Its contents are not read or logged. `--token-query` is not used.

Steam library registration is separate. `/usr/bin/steamos-add-to-steam` can be given `WebRTCsays.AI.desktop` later to open the add dialog. This package does not run that helper and does not register a manifest.

## Controls

Launch `webrtcsays_app.sh`. The overlay shows WebRTCsays.AI, Connect, and Disconnect.

- Connect starts one directcall on cores 2, 3, 4, and 7. The still image is sent at 1920x1080 and 30 fps for at most 15 minutes. A second Connect while that process is still the same pid does not start another copy.
- Disconnect sends SIGTERM only after the pid, start time, executable, and full argv still match. If they do not, the panel says the identity changed and does not signal. If the process does not exit, SIGKILL is sent to that same verified pid.
- Closing the app, or SIGTERM to the panel, stops the owned call.
- Temperature at or above 80C for 6 seconds, or resident memory at or above 7 GiB, stops the owned call and leaves that reason on the panel.

The process line can say running while send, receive, and audio still say `not measured`. Codec and resolution appear only after a stats line reports a positive frame size. `transport: process running, no stats yet` is not a connected call. `transport: ice connected` is the measured ICE state.

## Checks

```sh
PYTHONPATH=. python3 -m unittest discover -s tests -v
```
