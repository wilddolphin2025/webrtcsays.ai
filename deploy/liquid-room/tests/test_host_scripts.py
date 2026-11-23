"""Host scripts: dry run by default, backups outside the web root, idempotent include."""
from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

HOST = Path(__file__).resolve().parents[1] / "host"


def run(script: str, env: dict[str, str]) -> subprocess.CompletedProcess:
    return subprocess.run(["bash", str(HOST / script)], env={**os.environ, **env}, capture_output=True, text=True)


class WwwScriptTests(unittest.TestCase):
    def test_dry_run_then_apply_with_backup(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root, backups = Path(tmp, "public_html"), Path(tmp, "site-backups")
            root.mkdir()
            (root / "demo.html").write_text("old placeholder\n")
            env = {"SITE_ROOT": str(root), "SITE_BACKUP_DIR": str(backups)}
            r = run("install-www-demo.sh", env)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("dry run", r.stdout)
            self.assertEqual((root / "demo.html").read_text(), "old placeholder\n")
            r = run("install-www-demo.sh", {**env, "INSTALL_WWW_DEMO_CONFIRM": "replace-demo-html"})
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn('id="demo-redirect"', (root / "demo.html").read_text())
            saved = list(backups.iterdir())
            self.assertEqual(len(saved), 1)
            self.assertEqual(saved[0].read_text(), "old placeholder\n")
            self.assertEqual(oct(backups.stat().st_mode & 0o777), "0o700")
            self.assertEqual([p.name for p in root.iterdir()], ["demo.html"], "no temp or backup files left in the web root")

    def test_refuses_backup_inside_web_root(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            r = run("install-www-demo.sh", {"SITE_ROOT": str(root), "SITE_BACKUP_DIR": str(root / "bk"),
                                            "INSTALL_WWW_DEMO_CONFIRM": "replace-demo-html"})
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("inside the web root", r.stderr)


class RtcScriptTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        t = Path(self.tmp.name)
        self.nginx = t / "nginx"
        (self.nginx / "sites-enabled").mkdir(parents=True)
        self.site = self.nginx / "sites-enabled" / "rtc.conf"
        self.site.write_text("server {\n    include /etc/nginx/snippets/webrtcsays-demo-locations.conf;\n"
                             "    include /etc/nginx/snippets/frame-view-locations.conf;\n"
                             "    include /etc/nginx/snippets/babelfish.conf;\n    location / { }\n}\n")
        self.gate = t / "gate.hex"
        self.env = {"RTC_SITE": str(self.site), "NGINX_DIR": str(self.nginx), "BACKUP_DIR": str(t / "bk"),
                    "ROOM_PAGE_DIR": str(t / "srv"), "GATE_HEX_FILE": str(self.gate), "PATH": os.environ["PATH"]}

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_dry_run_changes_nothing(self) -> None:
        before = self.site.read_text()
        r = run("install-room-rtc.sh", self.env)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("dry run", r.stdout)
        self.assertEqual(self.site.read_text(), before)
        self.assertFalse((self.nginx / "liquid-gate.conf").exists())

    def test_refuses_site_without_demo_include(self) -> None:
        self.site.write_text("server { location / { } }\n")
        r = run("install-room-rtc.sh", self.env)
        self.assertNotEqual(r.returncode, 0)

    @unittest.skipUnless(os.geteuid() == 0, "apply path needs root (chown root)")
    def test_apply_inserts_once_after_frame_view(self) -> None:
        r = run("make-gate-secret.sh", self.env)
        self.assertEqual(r.returncode, 0, r.stderr)
        value = self.gate.read_text().strip()
        self.assertNotIn(value, r.stdout)
        env = {**self.env, "INSTALL_ROOM_CONFIRM": "room-locations"}
        for _ in range(2):
            r = run("install-room-rtc.sh", env)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertNotIn(value, r.stdout + r.stderr)
        lines = self.site.read_text().splitlines()
        inc = [i for i, l in enumerate(lines) if "liquid-room-locations.conf" in l]
        self.assertEqual(len(inc), 1)
        self.assertIn("frame-view-locations.conf", lines[inc[0] - 1])
        gate_conf = self.nginx / "liquid-gate.conf"
        self.assertEqual(oct(gate_conf.stat().st_mode & 0o777), "0o600")
        self.assertEqual(gate_conf.read_text(), f'proxy_set_header X-Liquid-Gate "{value}";\n')


if __name__ == "__main__":
    unittest.main()
