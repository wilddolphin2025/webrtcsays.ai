#!/usr/bin/env python3
"""Host templates, page contract, and the unsafe deploy-script guard."""

from __future__ import annotations

import json
import os
import re
import stat
import subprocess
import sys
import tarfile
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
ROOT = Path(__file__).resolve().parents[1]


class HostContractTests(unittest.TestCase):
    def test_page_uses_hostname_and_has_no_raw_ip(self) -> None:
        page = (ROOT / "page" / "index.html").read_text(encoding="utf-8")
        self.assertIn("room101", page)
        self.assertIn("Slim", page)
        self.assertIn('playsinline', page)
        self.assertIn("/demo/turn", page)
        self.assertIn("/demo/ws", page)
        self.assertIn("/demo/logout", page)
        self.assertIn("location.host", page)
        self.assertIn("no-referrer", page)
        self.assertNotRegex(page, r"\b(?:\d{1,3}\.){3}\d{1,3}\b")
        self.assertNotIn("ws://127.0.0.1", page)
        self.assertNotIn("<turn-password>", page)
        self.assertNotRegex(page, r"credential\s*[:=]\s*[\"'][^\"']+[\"']")
        self.assertIn("#1B2838", page)
        self.assertIn("14px/1.4", page)
        self.assertIn('id="startBtn"', page)
        self.assertIn('id="remoteVideo"', page)
        self.assertNotIn("steampowered", page.lower())
        self.assertNotIn('class="steam"', page)

    def test_dockerfile_is_exec_form_and_compose_uses_init(self) -> None:
        docker = (ROOT / "auth" / "Dockerfile").read_text(encoding="utf-8")
        compose = (ROOT / "auth" / "docker-compose.yml").read_text(encoding="utf-8")
        install = (ROOT / "host" / "install-auth.sh").read_text(encoding="utf-8")
        self.assertIn('CMD ["python"', docker)
        self.assertNotIn("CMD python", docker)
        self.assertIn("EXPOSE 8793", docker)
        self.assertIn("init: true", compose)
        self.assertIn("127.0.0.1:8793:8793", compose)
        self.assertIn("--init", install)
        self.assertIn("127.0.0.1:8793:8793", install)
        self.assertNotIn("stage1pass", docker + compose + install)

    def test_nginx_templates_do_not_log_queries(self) -> None:
        http_conf = (ROOT / "nginx" / "webrtcsays-demo-http.conf").read_text(encoding="utf-8")
        locations = (ROOT / "nginx" / "webrtcsays-demo-locations.conf").read_text(encoding="utf-8")
        self.assertIn("demo_noargs", http_conf)
        active = "\n".join(line for line in http_conf.splitlines() if not line.strip().startswith("#"))
        self.assertNotRegex(active, r"\$request(?!_method)")
        self.assertNotIn("$args", active)
        self.assertNotIn("$http_authorization", active)
        self.assertNotIn("$http_cookie", active)
        self.assertNotIn("sites-enabled/default", locations)
        self.assertNotIn("server_name _", locations)
        self.assertIn("rewrite ^ /? break;", locations)
        self.assertIn("proxy_http_version 1.1;", locations)
        self.assertIn("auth_request /_demo_auth;", locations)
        self.assertIn("return 302 /demo/login?next=$demo_log_uri;", locations)
        self.assertNotIn("$request_uri", locations)
        http_active = "\n".join(
            line for line in http_conf.splitlines() if not line.strip().startswith("#")
        )
        self.assertNotIn("limit_req_status", http_active)
        self.assertEqual(locations.count("limit_req_status 429;"), 2)
        self.assertEqual(locations.count('add_header Cache-Control "no-store, private" always;'), 3)

    def test_stage_nginx_is_additive_and_refuses_default_site(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            site = root / "rtc.wilddolphin.us.conf"
            site.write_text(textwrap.dedent("""
                server {
                    listen 443 ssl;
                    server_name rtc.wilddolphin.us;
                    location = /ws {
                        proxy_pass http://127.0.0.1:3459;
                        proxy_http_version 1.1;
                        proxy_set_header Upgrade $http_upgrade;
                        proxy_set_header Connection "upgrade";
                    }
                    location /ws/ {
                        proxy_pass http://127.0.0.1:8787;
                    }
                    location /karaoke/ {
                        proxy_pass http://127.0.0.1:8790;
                    }
                    include /etc/nginx/snippets/babelfish.conf;
                    location / {
                        proxy_pass http://127.0.0.1:8787;
                    }
                }
            """).strip() + "\n", encoding="utf-8")
            original = site.read_text(encoding="utf-8")
            script = ROOT / "host" / "nginx_stage.py"
            common = [
                sys.executable, str(script),
                "--site", str(site),
                "--nginx-dir", str(root / "nginx"),
                "--backup-dir", str(root / "backups"),
                "--locations", str(ROOT / "nginx" / "webrtcsays-demo-locations.conf"),
                "--http-conf", str(ROOT / "nginx" / "webrtcsays-demo-http.conf"),
            ]
            check = subprocess.run(common, check=False, capture_output=True, text=True)
            self.assertEqual(check.returncode, 0, check.stderr)
            self.assertEqual(site.read_text(encoding="utf-8"), original)
            default_site = root / "default"
            default_site.write_text(original, encoding="utf-8")
            refused = subprocess.run(
                common[:3] + [str(default_site)] + common[4:],
                check=False, capture_output=True, text=True,
            )
            self.assertNotEqual(refused.returncode, 0)
            self.assertIn("default", refused.stderr)
            self.assertEqual(default_site.read_text(encoding="utf-8"), original)
            applied = subprocess.run(common + ["--apply"], check=False, capture_output=True, text=True)
            self.assertEqual(applied.returncode, 0, applied.stderr)
            updated = site.read_text(encoding="utf-8")
            self.assertIn("include /etc/nginx/snippets/webrtcsays-demo-locations.conf;", updated)
            self.assertIn("include /etc/nginx/snippets/babelfish.conf;", updated)
            self.assertLess(
                updated.index("webrtcsays-demo-locations.conf"),
                updated.index("babelfish.conf"),
            )
            self.assertIn("location /ws/", updated)
            self.assertIn("127.0.0.1:8787", updated)
            self.assertIn("location / {", updated)
            self.assertIn("location /karaoke/", updated)
            active = "\n".join(
                line for line in updated.splitlines() if line.strip() and not line.strip().startswith("#")
            )
            self.assertNotIn("location = /ws {", active)
            again = subprocess.run(common + ["--apply"], check=False, capture_output=True, text=True)
            self.assertEqual(again.returncode, 0, again.stderr)
            self.assertEqual(updated.count("webrtcsays-demo-locations.conf"), site.read_text(encoding="utf-8").count("webrtcsays-demo-locations.conf"))
            self.assertTrue((root / "nginx" / "snippets" / "webrtcsays-demo-locations.conf").is_file())
            self.assertTrue((root / "nginx" / "conf.d" / "webrtcsays-demo-http.conf").is_file())
            backups = list(root.rglob("*.bak-demo-*"))
            self.assertTrue(backups)
            self.assertTrue(all(path.parent == root / "backups" for path in backups))

    def test_nginx_backup_stays_out_of_sites_enabled(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            tree = root / "nginx"
            available = tree / "sites-available"
            enabled = tree / "sites-enabled"
            available.mkdir(parents=True)
            enabled.mkdir()
            body = textwrap.dedent("""
                server {
                    listen 127.0.0.1:18080;
                    server_name rtc.wilddolphin.us;
                    location = /ws {
                        proxy_pass http://127.0.0.1:3459;
                    }
                    location /ws/ {
                        return 200 "signal\\n";
                    }
                    include /etc/nginx/snippets/babelfish.conf;
                    location / {
                        return 200 "gateway\\n";
                    }
                }
            """).strip() + "\n"
            real = available / "rtc.wilddolphin.us.conf"
            real.write_text(body, encoding="utf-8")
            link = enabled / "rtc.wilddolphin.us.conf"
            link.symlink_to("../sites-available/rtc.wilddolphin.us.conf")
            backups = root / "backups"
            script = ROOT / "host" / "nginx_stage.py"
            refused = subprocess.run(
                [sys.executable, str(script), "--site", str(link), "--nginx-dir", str(tree),
                 "--backup-dir", str(enabled / "oops"),
                 "--locations", str(ROOT / "nginx" / "webrtcsays-demo-locations.conf"),
                 "--http-conf", str(ROOT / "nginx" / "webrtcsays-demo-http.conf"),
                 "--apply"],
                check=False, capture_output=True, text=True,
            )
            self.assertNotEqual(refused.returncode, 0)
            self.assertEqual(real.read_text(encoding="utf-8"), body)
            applied = subprocess.run(
                [sys.executable, str(script), "--site", str(link), "--nginx-dir", str(tree),
                 "--backup-dir", str(backups),
                 "--locations", str(ROOT / "nginx" / "webrtcsays-demo-locations.conf"),
                 "--http-conf", str(ROOT / "nginx" / "webrtcsays-demo-http.conf"),
                 "--apply"],
                check=False, capture_output=True, text=True,
            )
            self.assertEqual(applied.returncode, 0, applied.stderr)
            self.assertTrue(link.is_symlink())
            self.assertEqual(link.resolve(), real.resolve())
            self.assertNotEqual(real.read_text(encoding="utf-8"), body)
            self.assertFalse(list(enabled.glob("*.bak*")))
            self.assertFalse(list(available.glob("*.bak*")))
            saved = list(backups.glob("*.bak-demo-*"))
            self.assertEqual(len(saved), 1)
            self.assertEqual(saved[0].read_text(encoding="utf-8"), body)
            self.assertEqual(stat.S_IMODE(saved[0].stat().st_mode), 0o600)
            included = sorted(path.name for path in enabled.iterdir())
            self.assertEqual(included, ["rtc.wilddolphin.us.conf"])
            chunks = []
            for path in sorted(enabled.iterdir(), key=lambda item: item.name):
                chunks.append("\n".join(
                    line for line in path.read_text(encoding="utf-8").splitlines()
                    if not line.strip().startswith("include ")
                ))
            server_text = "\n".join(chunks)
            prefix = root / "prefix"
            for name in ("client_body", "proxy", "fastcgi", "uwsgi", "scgi"):
                (prefix / name).mkdir(parents=True)
            conf = prefix / "nginx.conf"
            conf.write_text(
                "worker_processes 1;\n"
                f"error_log {prefix / 'error.log'} info;\n"
                f"pid {prefix / 'nginx.pid'};\n"
                "events { worker_connections 16; }\n"
                "http {\n"
                "  access_log off;\n"
                f"  client_body_temp_path {prefix / 'client_body'};\n"
                f"  proxy_temp_path {prefix / 'proxy'};\n"
                f"  fastcgi_temp_path {prefix / 'fastcgi'};\n"
                f"  uwsgi_temp_path {prefix / 'uwsgi'};\n"
                f"  scgi_temp_path {prefix / 'scgi'};\n"
                f"{server_text}\n"
                "}\n",
                encoding="utf-8",
            )
            checked = subprocess.run(
                ["nginx", "-p", str(prefix), "-c", str(conf), "-t"],
                check=False, capture_output=True, text=True,
            )
            self.assertEqual(checked.returncode, 0, checked.stderr)
            self.assertNotIn("conflicting server name", checked.stderr)

    def test_prepare_config_preserves_turns_without_printing_them(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            secret = "super-secret-turn-value"
            source = root / "config.talkingface5.json"
            dest = root / "config.demo.json"
            source.write_text(json.dumps({
                "mode": "callee",
                "address": "0.0.0.0:3458",
                "user_name": "Slim",
                "room_name": "room101",
                "websocket_port": 3459,
                "turns": [["turn:turn.wilddolphin.us:3478?transport=udp", "static-user", secret]],
                "whisper_model": "/opt/models/ggml-small.bin",
            }), encoding="utf-8")
            script = ROOT / "host" / "prepare_loopback_config.py"
            check = subprocess.run(
                [sys.executable, str(script), "--src", str(source), "--dst", str(dest)],
                check=False, capture_output=True, text=True,
            )
            self.assertEqual(check.returncode, 0, check.stderr)
            self.assertNotIn(secret, check.stdout)
            self.assertFalse(dest.exists())
            applied = subprocess.run(
                [sys.executable, str(script), "--src", str(source), "--dst", str(dest), "--apply"],
                check=False, capture_output=True, text=True,
                env={**os.environ, "PREPARE_DEMO_CONFIG_CONFIRM": "copy-loopback"},
            )
            self.assertEqual(applied.returncode, 0, applied.stderr)
            self.assertNotIn(secret, applied.stdout)
            data = json.loads(dest.read_text(encoding="utf-8"))
            self.assertEqual(data["address"], "127.0.0.1:3458")
            self.assertEqual(data["signaling_bind"], "127.0.0.1")
            self.assertIn("address alone does not bind", applied.stdout)
            self.assertIn("PR 18", applied.stdout)
            self.assertEqual(data["websocket_port"], 3459)
            self.assertEqual(data["turns"][0][2], secret)
            self.assertEqual(data["user_name"], "Slim")
            self.assertEqual(stat.S_IMODE(dest.stat().st_mode), 0o600)

    def test_retire_page_backs_up_then_deletes_without_printing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            web = root / "www"
            web.mkdir()
            target = web / "index.html"
            secret = "embedded-turn-secret-value"
            target.write_text(f"<html>{secret}</html>", encoding="utf-8")
            backup = root / "private"
            script = ROOT / "host" / "retire-old-demo-page.sh"
            refused = subprocess.run(
                ["bash", str(script)],
                check=False, capture_output=True, text=True,
                env={**os.environ, "DEMO_PAGE_TARGET": str(target), "DEMO_PAGE_BACKUP_DIR": str(backup)},
            )
            self.assertNotEqual(refused.returncode, 0)
            self.assertTrue(target.is_file())
            web_backup = web / "backup"
            leaked = subprocess.run(
                ["bash", str(script)],
                check=False, capture_output=True, text=True,
                env={
                    **os.environ,
                    "DEMO_PAGE_TARGET": str(target),
                    "DEMO_PAGE_BACKUP_DIR": str(web_backup),
                    "RETIRE_DEMO_PAGE_CONFIRM": "backup-then-delete",
                },
            )
            self.assertNotEqual(leaked.returncode, 0)
            self.assertTrue(target.is_file())
            done = subprocess.run(
                ["bash", str(script)],
                check=False, capture_output=True, text=True,
                env={
                    **os.environ,
                    "DEMO_PAGE_TARGET": str(target),
                    "DEMO_PAGE_BACKUP_DIR": str(backup),
                    "RETIRE_DEMO_PAGE_CONFIRM": "backup-then-delete",
                },
            )
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertNotIn(secret, done.stdout)
            self.assertFalse(target.exists())
            copies = list(backup.glob("index.html.bak-*"))
            self.assertEqual(len(copies), 1)
            self.assertEqual(copies[0].read_text(encoding="utf-8"), f"<html>{secret}</html>")
            self.assertEqual(stat.S_IMODE(copies[0].stat().st_mode), 0o600)
            self.assertEqual(stat.S_IMODE(backup.stat().st_mode), 0o700)

    def test_capture_env_keeps_runtime_keys_only(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "environ"
            source.write_bytes(b"\0".join([
                b"LD_LIBRARY_PATH=/opt/directcall3-dev/lib",
                b"WHILLATS_SERVER=/opt/whillats/whillats_server",
                b"TURN_PASS=do-not-copy-this",
                b"SECRET_TOKEN=also-secret",
                b"HOME=/root",
            ]))
            out = root / "directcall.env"
            script = ROOT / "host" / "capture-directcall-env.sh"
            check = subprocess.run(
                ["bash", str(script)],
                check=False, capture_output=True, text=True,
                env={**os.environ, "DEMO_ENVIRON_FILE": str(source), "DEMO_ENV_OUT": str(out)},
            )
            self.assertEqual(check.returncode, 0, check.stderr)
            self.assertNotIn("do-not-copy-this", check.stdout)
            self.assertNotIn("/opt/directcall3-dev/lib", check.stdout)
            self.assertFalse(out.exists())
            applied = subprocess.run(
                ["bash", str(script), "--apply"],
                check=False, capture_output=True, text=True,
                env={**os.environ, "DEMO_ENVIRON_FILE": str(source), "DEMO_ENV_OUT": str(out)},
            )
            self.assertEqual(applied.returncode, 0, applied.stderr)
            text = out.read_text(encoding="utf-8")
            self.assertIn("LD_LIBRARY_PATH=/opt/directcall3-dev/lib", text)
            self.assertIn("WHILLATS_SERVER=/opt/whillats/whillats_server", text)
            self.assertNotIn("TURN_PASS", text)
            self.assertNotIn("SECRET_TOKEN", text)
            self.assertEqual(stat.S_IMODE(out.stat().st_mode), 0o600)

    def test_capture_env_allows_dead_process(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            out = root / "directcall.env"
            script = ROOT / "host" / "capture-directcall-env.sh"
            done = subprocess.run(
                ["bash", str(script), "--apply"],
                check=False, capture_output=True, text=True,
                env={
                    **os.environ,
                    "DEMO_ENV_OUT": str(out),
                    "DEMO_DIRECTCALL_PID": "999999",
                    "DEMO_ENVIRON_FILE": "",
                },
            )
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertIn("unavailable", done.stdout)
            self.assertIn("whisper", done.stdout)
            self.assertEqual(out.read_text(encoding="utf-8"), "")
            self.assertEqual(stat.S_IMODE(out.stat().st_mode), 0o600)

    def test_auth_secret_perms_are_container_uid(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            conf = root / "webrtcsays-demo"
            conf.mkdir()
            (conf / "session.key").write_text("k" * 48, encoding="utf-8")
            (conf / "password").write_text("browserpass1234\n", encoding="utf-8")
            (conf / "directcall.env").write_text("LD_LIBRARY_PATH=/opt/lib\n", encoding="utf-8")
            os.chmod(conf / "session.key", 0o644)
            os.chmod(conf / "directcall.env", 0o600)
            script = ROOT / "host" / "set-auth-perms.sh"
            refused = subprocess.run(
                ["bash", str(script), "--apply"],
                check=False, capture_output=True, text=True,
                env={**os.environ, "DEMO_AUTH_CONF": str(root / "sites-enabled" / "secrets")},
            )
            self.assertNotEqual(refused.returncode, 0)
            done = subprocess.run(
                ["bash", str(script), "--apply"],
                check=False, capture_output=True, text=True,
                env={
                    **os.environ,
                    "DEMO_AUTH_CONF": str(conf),
                    "DEMO_AUTH_UID": str(os.getuid()),
                    "DEMO_AUTH_GID": str(os.getgid()),
                },
            )
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertEqual(stat.S_IMODE(conf.stat().st_mode), 0o700)
            self.assertEqual(conf.stat().st_uid, os.getuid())
            self.assertEqual(stat.S_IMODE((conf / "session.key").stat().st_mode), 0o600)
            self.assertEqual((conf / "session.key").stat().st_uid, os.getuid())
            self.assertEqual(stat.S_IMODE((conf / "password").stat().st_mode), 0o600)
            self.assertEqual((conf / "directcall.env").read_text(encoding="utf-8"), "LD_LIBRARY_PATH=/opt/lib\n")
            self.assertIn("directcall.env is not changed", done.stdout)

    def test_install_auth_check_does_not_run_docker(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake = root / "docker"
            log = root / "docker.log"
            fake.write_text(f"#!/bin/sh\necho invoked >> {log}\n", encoding="utf-8")
            fake.chmod(0o755)
            script = ROOT / "host" / "install-auth.sh"
            done = subprocess.run(
                ["bash", str(script)],
                check=False, capture_output=True, text=True,
                env={**os.environ, "PATH": f"{root}:{os.environ.get('PATH', '')}"},
            )
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertIn("--init", done.stdout)
            self.assertIn("127.0.0.1:8793:8793", done.stdout)
            self.assertIn("check only", done.stderr)
            self.assertFalse(log.exists())

    def test_deploy_script_refuses_shared_vps_without_ssh(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bindir = root / "bin"
            bindir.mkdir()
            ssh_log = root / "ssh.log"
            ssh = bindir / "ssh"
            ssh.write_text(f"#!/bin/sh\necho invoked >> {ssh_log}\nexit 99\n", encoding="utf-8")
            ssh.chmod(0o755)
            key = root / "key"
            key.write_text("not-a-real-key\n", encoding="utf-8")
            script = REPO / "scripts" / "deploy" / "deploy_directcall_remote.sh"
            done = subprocess.run(
                ["bash", str(script)],
                check=False, capture_output=True, text=True, timeout=10,
                env={
                    "PATH": f"{bindir}:{os.environ.get('PATH', '')}",
                    "HOME": str(root),
                    "SSH_KEY_PATH": str(key),
                    "DEPLOY_HOST": "217.77.3.65",
                    "DEPLOY_USER": "root",
                    "TURN_USER": "unused",
                    "TURN_PASS": "unused",
                },
            )
            self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
            self.assertIn("REFUSING", done.stderr)
            self.assertFalse(ssh_log.exists())
            dry = subprocess.run(
                ["bash", str(script)],
                check=False, capture_output=True, text=True, timeout=10,
                env={
                    "PATH": f"{bindir}:{os.environ.get('PATH', '')}",
                    "HOME": str(root),
                    "SSH_KEY_PATH": str(key),
                    "DEPLOY_HOST": "217.77.3.65",
                    "DEPLOY_DRY_RUN": "true",
                    "TURN_USER": "unused",
                    "TURN_PASS": "unused",
                },
            )
            self.assertEqual(dry.returncode, 0, dry.stderr)
            self.assertFalse(ssh_log.exists())

    def test_deploy_md_has_no_inline_credential_pair(self) -> None:
        text = (REPO / "DEPLOY.md").read_text(encoding="utf-8")
        self.assertNotRegex(text, r"Credentials:\s*`[^`]+`\s*/\s*`[^`]+`")
        self.assertIn("must not be reused", text)
        self.assertIn("unsafe", text.lower())

    def test_package_contains_only_the_demo_directory(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            archive = Path(tmp) / "demo.tar.gz"
            script = ROOT / "package-artifact.sh"
            done = subprocess.run(["bash", str(script), str(archive)], check=False, capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stderr)
            with tarfile.open(archive) as bundle:
                names = bundle.getnames()
            normalized = [name[2:] if name.startswith("./") else name for name in names]
            self.assertTrue(all(
                name == "webrtcsays-demo" or name.startswith("webrtcsays-demo/")
                for name in normalized
            ))
            self.assertFalse(any(name.startswith("modules/") or ".git" in name.split("/") for name in names))
            self.assertTrue(any(name.endswith("auth/demo_auth.py") for name in names))
            self.assertTrue(any(name.endswith("page/index.html") for name in names))


if __name__ == "__main__":
    unittest.main()
