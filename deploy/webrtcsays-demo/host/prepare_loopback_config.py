#!/usr/bin/env python3
"""Copy the installed directcall JSON and set only the TCP address.

The WebSocket and TCP sockets in the current binary still bind 0.0.0.0.
The address field is what the in-process bridge dials. This copy sets it
to 127.0.0.1 and leaves every other key, including turns, unchanged.
Stdout lists key names only. Default mode does not write.
"""

from __future__ import annotations

import argparse
import json
import os
import stat
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="Prepare a loopback directcall config copy")
    parser.add_argument("--src", default=os.environ.get("DEMO_CONFIG_SRC", "/opt/directcall3-dev/config.talkingface5.json"))
    parser.add_argument("--dst", default=os.environ.get("DEMO_CONFIG_DST", "/opt/directcall3-dev/config.demo.json"))
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    source = Path(args.src)
    data = json.loads(source.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        sys.stderr.write("config is not an object\n")
        return 1
    original_address = data.get("address")
    data["address"] = "127.0.0.1:3458"
    if "websocket_port" not in data:
        data["websocket_port"] = 3459
    sys.stdout.write("address -> 127.0.0.1:3458\n")
    sys.stdout.write("preserved keys: " + ", ".join(sorted(data.keys())) + "\n")
    if isinstance(original_address, str):
        sys.stdout.write("previous address was replaced\n")
    if not args.apply:
        sys.stdout.write("check only: destination not written\n")
        return 0
    if os.environ.get("PREPARE_DEMO_CONFIG_CONFIRM") != "copy-loopback":
        sys.stderr.write("refusing to write without PREPARE_DEMO_CONFIG_CONFIRM=copy-loopback\n")
        return 1
    dest = Path(args.dst)
    payload = json.dumps(data, indent=2) + "\n"
    dest.write_text(payload, encoding="utf-8")
    os.chmod(dest, stat.S_IRUSR | stat.S_IWUSR)
    sys.stdout.write(f"wrote {dest.name} mode 0600\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
