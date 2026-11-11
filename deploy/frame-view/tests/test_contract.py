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
            "view-disconnect",
            "view-status",
            "view-codec",
            "view-remote",
            "view-log",
        ):
            self.assertIn(f'id="{element_id}"', page)
        self.assertIn("#1B2838", css)
        self.assertIn("14px/1.4", css)
        self.assertNotIn("#101418", css)
        self.assertNotIn("getUserMedia", (ROOT / "page" / "view.js").read_text(encoding="utf-8"))

    def test_nginx_adds_viewer_routes_only(self) -> None:
        conf = (ROOT / "nginx" / "frame-view-locations.conf").read_text(encoding="utf-8")
        self.assertIn("auth_request /_demo_auth;", conf)
        self.assertIn("demo_noargs", conf)
        self.assertIn("127.0.0.1:3461", conf)
        self.assertIn("X-Original-URI $demo_orig_uri", conf)
        self.assertIn('X-Demo-Auth "ok"', conf)
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
