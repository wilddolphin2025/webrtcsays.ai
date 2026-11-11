# WebRTCsays.AI call

A fullscreen call view for the Frame. The remote picture fills the view. Connect and Hangup are large controls on that same image, with status and a local still preview. This is not a small utility panel and not a browser page.

The image is one head-locked OpenVR surface, about 102 degrees wide (2.6 m at 1.05 m). It is not an eye-buffer `IVRCompositor.Submit`, so the far edges of the headset can still show the VR room. An X11 or SDL fullscreen window is not used as the headset view. That difference has not been checked by a person wearing the Frame.

directcall writes remote frames to `remote.frame` with `--remote-window=app` and does not open its own `webrtcsays.remote` overlay. The call app draws those frames into the main view. Nothing is connected until Connect.

No desktop entry is installed, nothing is registered with Steam, and nothing is set to autostart.

## Install

On the Frame, from this directory:

```sh
PYTHONPATH=. python3 - << 'PY'
from pathlib import Path
from webrtcsays_panel.install import install_tree
install_tree(Path(".").resolve(), Path.home(), "manual")
PY
chmod 755 "$HOME/babelfish/call/app/webrtcsays_app.sh"
```

That writes only under `~/babelfish/call/app`. It does not create or edit files in `~/.local/share/applications`, autostart, or Steam.

These files are already on the Frame and are not in this repo:

- `~/babelfish/call/direct/pr21/directcall` (build that accepts `--remote-window=app`)
- `~/babelfish/call/direct/pr21/VR-Babelfish-1080p.png`
- `~/babelfish/call/direct/pr21/token`

Run the app by executing `~/babelfish/call/app/webrtcsays_app.sh` while SteamVR is already running. The token file is passed with `--token-file` and is not read or logged.

## Controls

The view shows WebRTCsays.AI, the remote picture, a local still preview, Connect, and Hangup.

- Connect starts one directcall on cores 2, 3, 4, and 7. The still is sent at 1920x1080 and 30 fps for at most 15 minutes. Launch does not start the call. Connect again while that same process is alive does not start another.
- Hangup checks the pid, start time, executable, and full argv, then SIGTERM. SIGKILL follows only if it is still that process.
- Closing the app stops the owned call.
- At or above 80C for 6 seconds, or resident memory at or above 7 GiB, stops the owned call.

`process: running` is not a connected call. Send and receive stay `not measured` until stats report a positive frame size.

## What was run here

```sh
PYTHONPATH=. python3 -m unittest discover -s tests -v
```

The tests mock process ownership, stats, guards, and button hits, and they draw the call image. No headset session was started.

`WebRTCsays.AI.desktop` in this directory is an unused template. It is not installed.
