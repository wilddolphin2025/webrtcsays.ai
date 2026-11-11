# WebRTCsays.AI call

A fullscreen call view for the Frame. The remote picture fills the view. Connect and Hangup are large controls on that same image, with status and a local still preview. This is not a small utility panel and not a browser page.

The image is one OpenVR overlay about 102 degrees wide (2.6 m at 1.05 m in front of the headset). Its pose is `SetOverlayTransformTrackedDeviceRelative` on the HMD, so it is meant to stay in front of the face. Remote video is fitted to the full 1920x1080 plane: a 16:9 frame fills it, and any other ratio is centered with opaque bars. Connect is `(646, 808, 300, 84)`, Hangup is `(974, 808, 300, 84)`, and the status line is `(646, 762, 628, 36)`, drawn on the video. The local chip is `(1656, 24, 240, 135)`. This is still not an `IVRCompositor.Submit` of the eye framebuffers, so it is not full-headset-framebuffer fullscreen and the far edges of the view can show the VR room. Laser clicks also set `VROverlayFlags_MakeOverlaysInteractiveIfVisible`. A person in the headset has not confirmed the lock, the laser, or that the centered controls are comfortable.

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

The sender directory is not hardcoded. Write it once:

```sh
printf '%s\n' "$HOME/babelfish/call/direct/pr22" > "$HOME/babelfish/call/app/direct.root"
```

That directory holds `directcall`, `VR-Babelfish-1080p.png`, and `token`. `WEBRTCSAYS_DIRECT_ROOT` overrides the file. An empty setting does not fall back to `pr21`.

Run the app by executing `~/babelfish/call/app/webrtcsays_app.sh` while SteamVR is already running. The token file is passed with `--token-file` and is not read or logged.

Connect reads that same token file and GETs `https://rtc.wilddolphin.us/demo/turn`. The response field is `uris`. directcall reads `urls`, so the app writes `~/babelfish/call/app/turn.json` and passes `--turns-file` with that path. The directory is mode `0700` and the file is mode `0600`. The username and credential are not command-line arguments and are not logged. Only `rtc.wilddolphin.us` TURN URIs are kept. TLS verification is unchanged. A missing or rejected account still starts signaling and shows `TURN not configured`. `WEBRTCSAYS_ICE_POLICY=relay` adds `--ice-policy=relay` so the call gathers TURN candidates only. The default remains all candidate types. Neither setting is a measurement that media or port 3478 worked.

## Controls

The view shows WebRTCsays.AI, the remote picture, a local still preview, Connect, and Hangup.

- Connect starts one directcall on cores 2, 3, 4, and 7. The command passes `--video` explicitly, with the still at 1920x1080 and 30 fps, for at most 15 minutes. It does not pass `--audio=dummy`, so the caller's default audio track stays enabled. Launch does not start the call. Connect again while that same process is alive does not start another.
- Hangup checks the pid, start time, executable, and full argv, then SIGTERM. SIGKILL follows only if it is still that process. directcall's own shutdown budget is 10 seconds, so a 5 second wait can still be followed by SIGKILL. That signal is only the owned caller. Stopping the panel hides and destroys the OpenVR overlay, then shuts down the OpenVR client, and only then waits on the caller. A Steam library shortcut may launch the app. It is not an autostart entry.
- Closing the app stops the owned call.
- The guard uses the hottest thermal zone, not only zone0. At or above 80C for 6 seconds, or the panel or the call at or above 7 GiB RSS, stops the owned call and exits the panel.
- While the picture is unchanged, the overlay image is uploaded once. Each new remote frame is fitted into that same 1920x1080 image and uploaded. There is no 4 fps cap. The command does not pass `--remote-slot`, so the installed writer keeps the received size instead of stretching it to 1920x820. The app fits that frame. Buttons are copied back on top of each frame. The native source also fits a slot when one is passed; the installed `pr22-0f1a7017` binary does not contain that writer change, and this app does not need a new binary.

`process: running` is not a connected call. Send and receive stay `not measured` until stats report a positive frame size.

## What was run here

```sh
PYTHONPATH=. python3 -m unittest discover -s tests -v
```

On this machine, copying a moving 1920x820 frame into the call image and into the upload buffer measured about 1140 fps (0.87 ms). Scaling a 1920x1080 frame into that rectangle measured about 67 fps. Both are above 30 fps here. The Frame CPU and the OpenVR `SetOverlayRaw` cost are not in those numbers. A person has not watched a 1080p30 call in the headset.

`WebRTCsays.AI.desktop` in this directory is an unused template. It is not installed.
