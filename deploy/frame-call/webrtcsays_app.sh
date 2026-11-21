#!/bin/sh
# Start the headset control panel. Does not stop unrelated processes.
set -eu
cd "$HOME/babelfish/call/app"
exec "$HOME/babelfish/venv/bin/python3.12" -B "$HOME/babelfish/call/app/webrtcsays_call.py"
