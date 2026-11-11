#!/usr/bin/env python3
"""Insert the stage-1 demo nginx snippet into the rtc server file.

Default mode is a dry run. --apply writes a backup and edits only the site
file you name. It does not reload nginx, delete sites-enabled/default, or
touch any other vhost. Do not point it at a production file from CI.
"""

from __future__ import annotations

import argparse
import shutil
import sys
import time
from pathlib import Path

INCLUDE = "include /etc/nginx/snippets/webrtcsays-demo-locations.conf;"
BABELFISH = "include /etc/nginx/snippets/babelfish.conf;"


def brace_end(text: str, open_at: int) -> int:
    depth = 0
    for index in range(open_at, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return index + 1
    raise SystemExit("unbalanced braces in the site file")


def location_blocks(text: str) -> list[tuple[int, int, str]]:
    blocks = []
    cursor = 0
    while True:
        start = text.find("location", cursor)
        if start < 0:
            break
        line_start = text.rfind("\n", 0, start) + 1
        if "#" in text[line_start:start] or (start > 0 and text[start - 1] not in "\n\t "):
            cursor = start + len("location")
            continue
        open_at = text.find("{", start)
        if open_at < 0:
            break
        header = " ".join(text[start:open_at].split())
        end = brace_end(text, open_at)
        blocks.append((start, end, header))
        cursor = end
    return blocks


def comment_span(text: str, start: int, end: int) -> str:
    body = text[start:end]
    commented = "\n".join("# " + line if line.strip() else "#" for line in body.splitlines())
    return text[:start] + commented + text[end:]


def retire_legacy_ws(text: str) -> tuple[str, int]:
    retired = 0
    blocks = location_blocks(text)
    for start, end, header in reversed(blocks):
        if header != "location = /ws":
            continue
        body = text[start:end]
        if "auth_request" in body:
            continue
        if "3459" not in body or "proxy_pass" not in body:
            continue
        note = (
            "# stage-1 demo: the unauthenticated exact /ws block is retired.\n"
            "# /ws/ (the signal gateway) is untouched. /ws now uses the demo gate.\n"
        )
        text = text[:start] + note + comment_span(body, 0, len(body)) + text[end:]
        retired += 1
    return text, retired


def insert_include(text: str) -> str:
    if INCLUDE in text:
        return text
    line = "    " + INCLUDE + "\n"
    if BABELFISH in text:
        return text.replace(BABELFISH, line + "    " + BABELFISH, 1)
    marker = None
    for candidate in ("\n    location / {", "\n\tlocation / {", "\nlocation / {"):
        if candidate in text:
            marker = candidate
            break
    if marker is None:
        raise SystemExit("refusing to edit: no babelfish include and no location /")
    return text.replace(marker, "\n" + line + marker, 1)


def refuse_path(site: Path) -> None:
    resolved = str(site.resolve())
    if site.name == "default" or resolved.endswith("/sites-enabled/default"):
        raise SystemExit("refusing to edit sites-enabled/default")
    if "sites-available/default" in resolved:
        raise SystemExit("refusing to edit sites-available/default")


def transform(text: str, server_name: str) -> str:
    if f"server_name {server_name}" not in text and f"server_name {server_name};" not in text:
        if server_name not in text:
            raise SystemExit(f"refusing to edit: {server_name} not in the site file")
    if "server_name _;" in text or "server_name _ " in text:
        raise SystemExit("refusing to edit a catch-all server_name _ file")
    updated, _retired = retire_legacy_ws(text)
    return insert_include(updated)


def copy_templates(nginx_dir: Path, locations: Path, http_conf: Path) -> None:
    snippet_dir = nginx_dir / "snippets"
    conf_dir = nginx_dir / "conf.d"
    snippet_dir.mkdir(parents=True, exist_ok=True)
    conf_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(locations, snippet_dir / "webrtcsays-demo-locations.conf")
    shutil.copyfile(http_conf, conf_dir / "webrtcsays-demo-http.conf")


def main() -> int:
    parser = argparse.ArgumentParser(description="Stage the demo nginx snippet")
    parser.add_argument("--site", required=True)
    parser.add_argument("--nginx-dir", required=True)
    parser.add_argument("--locations", required=True)
    parser.add_argument("--http-conf", required=True)
    parser.add_argument("--server-name", default="rtc.wilddolphin.us")
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    site = Path(args.site)
    refuse_path(site)
    original = site.read_text(encoding="utf-8")
    updated = transform(original, args.server_name)
    if not args.apply:
        sys.stdout.write("check only: site file not modified\n")
        if INCLUDE not in original:
            sys.stdout.write("would insert the demo include\n")
        if "location = /ws" in original and "auth_request" not in original.split("location = /ws", 1)[1].split("location", 1)[0]:
            sys.stdout.write("would comment the unauthenticated location = /ws block\n")
        return 0
    stamp = time.strftime("%Y%m%d%H%M%S")
    backup = site.with_name(site.name + f".bak-demo-{stamp}")
    backup.write_text(original, encoding="utf-8")
    site.write_text(updated, encoding="utf-8")
    copy_templates(Path(args.nginx_dir), Path(args.locations), Path(args.http_conf))
    sys.stdout.write(f"applied demo nginx snippet; backup {backup.name}\n")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SystemExit as exc:
        if isinstance(exc.code, str):
            sys.stderr.write(exc.code + "\n")
            sys.exit(1)
        raise
