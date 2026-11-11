#!/usr/bin/env python3
"""Static checks for the viewer bundle. No network and no PR 17 checkout."""

from __future__ import annotations

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
PR17 = "1dfe20dbc1088e6f51f74e2baf99c1201d23d0e2"


class ContractTests(unittest.TestCase):
    def test_readme_pins_the_palette_head(self) -> None:
        text = (ROOT / "README.md").read_text(encoding="utf-8")
        self.assertIn(PR17, text)
        self.assertIn("69289a61609c73b4a68b0244e43012e3c31d4c72", text)
        self.assertIn("demo-auth-view-paths.patch", text)
        self.assertNotIn("d715d3f9` on", text)

    def test_page_keeps_ids_and_finished_palette(self) -> None:
        page = (ROOT / "page" / "index.html").read_text(encoding="utf-8")
        css = (ROOT / "page" / "view.css").read_text(encoding="utf-8")
        for element_id in (
            "view-connect",
            "view-camera",
            "view-disconnect",
            "view-status",
            "view-codec",
            "view-send",
            "view-remote",
            "view-log",
        ):
            self.assertIn(f'id="{element_id}"', page)
        self.assertIn("#1B2838", css)
        self.assertIn("14px/1.4", css)
        self.assertNotIn("#101418", css)
        self.assertIn('<h1 class="wm">WebRTCsays.AI</h1>', page)
        self.assertIn("<title>Frame viewer</title>", page)
        self.assertNotIn("WebRTCSays", page)
        script = (ROOT / "page" / "view.js").read_text(encoding="utf-8")
        connect = script.split("async function connect()", 1)[1].split("async function ", 1)[0]
        self.assertNotIn("getDisplayMedia", connect)
        self.assertNotIn("getUserMedia", connect)
        self.assertIn("screen=unavailable", script)
        self.assertIn("screen=cancelled", script)
        self.assertIn("mic=cancelled", script)
        self.assertIn("getDisplayMedia", script)
        self.assertIn("getUserMedia({ audio: true, video: false })", script)
        self.assertIn('id="view-share"', page)
        self.assertIn('id="view-camera"', page)
        self.assertIn('id="view-mic"', page)
        self.assertIn('id="view-remote-audio"', page)
        self.assertIn("getUserMedia({", script)
        self.assertNotIn("getUserMedia", connect)
        self.assertNotIn("getDisplayMedia", connect)
        self.assertIn("payload.uris || payload.urls", script)
        self.assertIn('"rtc.wilddolphin.us": 1', script)
        self.assertNotIn("turn.wilddolphin.us", script)
        self.assertIn("idle-timeout", script)
        self.assertIn('failWait("disconnected")', script)
        self.assertNotIn("log(payload.credential", script)
        self.assertNotIn("log(payload.username", script)

    def test_nginx_adds_viewer_routes_only(self) -> None:
        conf = (ROOT / "nginx" / "frame-view-locations.conf").read_text(encoding="utf-8")
        self.assertIn("auth_request /_demo_auth;", conf)
        self.assertIn("demo_noargs", conf)
        self.assertIn("127.0.0.1:3461", conf)
        self.assertIn("X-Original-URI $demo_orig_uri", conf)
        self.assertIn('X-Demo-Auth "ok"', conf)
        self.assertIn("location ~ '^/demo/view/ws/", conf)
        self.assertIn("location ~ '^/demo/view/(?!ws$|assets$)", conf)
        self.assertNotIn("location ~ ^/demo/view/ws/", conf)
        import re
        for pattern in (
            r"^location = /demo/\s*\{",
            r"^location = /demo/ws\b",
            r"^location = /demo/turn\b",
            r"^location = /ws\b",
            r"^location /ws/",
            r"^location / \{",
        ):
            self.assertIsNone(re.search(pattern, conf, re.M), pattern)

    def test_auth_patch_targets_the_path_gate(self) -> None:
        patch = (ROOT / "auth" / "demo-auth-view-paths.patch").read_text(encoding="utf-8")
        self.assertIn("VIEW_PAGE_RE", patch)
        self.assertIn("VIEW_ASSET_RE", patch)
        self.assertIn("VIEW_WS_RE", patch)
        self.assertIn("view_path(path)", patch)
        self.assertIn("VIEW_PAGE_RE.fullmatch(decoded)", patch)
        self.assertNotIn("login_page", patch)
        self.assertNotIn("session_cookie", patch)
