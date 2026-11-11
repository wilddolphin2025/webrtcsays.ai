#!/usr/bin/env python3
"""Mocked ownership, media state, guards, and headset-panel controls."""

import os
import signal
import stat
import tempfile
import unittest
from pathlib import Path

from webrtcsays_panel.command import CORES, directcall_argv
from webrtcsays_panel.guards import ThermalGuard, memory_tripped, read_celsius, read_rss_bytes
from webrtcsays_panel.install import install_tree
from webrtcsays_panel.layout import HEIGHT, hit_button
from webrtcsays_panel.overlay_input import MOUSE_DOWN, clicks_from_events
from webrtcsays_panel.owner import ProcFs
from webrtcsays_panel.session import Session
from webrtcsays_panel.stats import MediaView, redact
from webrtcsays_panel import OVERLAY_KEY, REMOTE_VIDEO_OVERLAY_KEY


def _stat(pid, ticks):
    tail = " ".join(["0"] * 18)
    return "%s (directcall) R %s %s\n" % (pid, tail, ticks)


class FakeProc:
    def __init__(self, root):
        self.proc = ProcFs(root)
        self.root = Path(root)
        self.killed = []

    def add(self, pid, ticks, exe, argv):
        base = self.root / str(pid)
        if base.exists():
            for child in base.iterdir():
                child.unlink()
        else:
            base.mkdir(parents=True)
        (base / "stat").write_text(_stat(pid, ticks), encoding="utf-8")
        (base / "cmdline").write_bytes(b"\0".join(part.encode() for part in argv) + b"\0")
        os.symlink(exe, base / "exe")

    def kill(self, pid, sig):
        self.killed.append((pid, sig))
        if sig == signal.SIGKILL:
            base = self.root / str(pid)
            if base.exists():
                for child in base.iterdir():
                    child.unlink()
                base.rmdir()


class FakeChild:
    def __init__(self, pid, lines=None):
        self.pid = pid
        self._lines = list(lines or [])

    def drain(self):
        lines, self._lines = self._lines, []
        return lines


class Clock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class PanelTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.home = Path(self.tmp.name)
        self.direct = self.home / "direct"
        self.direct.mkdir()
        self.binary = self.direct / "directcall"
        self.binary.write_text("#!/bin/sh\n", encoding="utf-8")
        self.binary.chmod(self.binary.stat().st_mode | stat.S_IEXEC)
        self.image = self.direct / "still.png"
        self.image.write_bytes(b"png")
        self.token = self.direct / "token"
        self.token.write_text("super-secret-token\n", encoding="utf-8")
        self.proc_root = self.home / "proc"
        self.proc_root.mkdir()
        self.fake = FakeProc(self.proc_root)
        self.spawned = []
        self.clock = Clock()
        self.frame = self.home / "remote.frame"
        self.session = Session(
            {
                "binary": self.binary,
                "image": self.image,
                "token": self.token,
                "frame": self.frame,
            },
            self.fake.proc,
            self.spawn,
            self.affinity,
            self.fake.kill,
            self.clock,
            self.clock.sleep,
        )

    def tearDown(self):
        self.tmp.cleanup()

    def spawn(self, argv):
        self.spawned.append(list(argv))
        pid = 4000 + len(self.spawned)
        self.fake.add(pid, 1000 + pid, str(self.binary.resolve()), argv)
        return FakeChild(pid)

    def affinity(self, pid, cores):
        self.cores = (pid, set(cores))

    def test_command_is_bearer_file_and_still_image(self):
        argv = directcall_argv(self.binary, self.image, self.token, self.frame)
        text = " ".join(argv)
        self.assertIn("--token-file=" + str(self.token), argv)
        self.assertNotIn("super-secret-token", text)
        self.assertNotIn("--token-query", argv)
        self.assertIn("--res=1920x1080", argv)
        self.assertIn("--fps=30", argv)
        self.assertIn("--video-source=image", argv)
        self.assertIn("--remote-window=app", argv)
        self.assertIn("--remote-frame=" + str(self.frame), argv)
        self.assertIn("--duration=900", argv)
        self.assertNotIn("pkill", text)

    def test_connect_pins_cores_and_second_connect_does_not_spawn(self):
        self.assertEqual(self.session.connect(), "started")
        self.assertEqual(self.cores[1], set(CORES))
        self.assertEqual(self.session.connect(), "already-running")
        self.assertEqual(len(self.spawned), 1)
        self.assertEqual(self.session.process_text, "running")
        self.assertEqual(self.session.media.transport(True), "process running, no stats yet")
        self.assertEqual(self.session.media.send_text(), "not measured")

    def test_disconnect_refuses_a_reused_pid(self):
        self.session.connect()
        pid = self.session.identity.pid
        self.fake.add(pid, 999999, str(self.binary.resolve()), ["other"])
        self.assertEqual(self.session.disconnect(), "refused")
        self.assertEqual(self.fake.killed, [])
        self.assertIn("identity changed", self.session.error)

    def test_disconnect_signals_only_the_owned_pid(self):
        self.session.connect()
        pid = self.session.identity.pid
        self.fake.kill = lambda p, sig: self.fake.killed.append((p, sig)) or None
        # Keep the process alive through SIGTERM so the wait observes it, then
        # the test clock advances and SIGKILL is the hang-safe follow-up.
        original = self.fake.killed

        def killer(p, sig):
            original.append((p, sig))
            if sig == signal.SIGKILL:
                base = self.proc_root / str(p)
                if base.exists():
                    for child in list(base.iterdir()):
                        child.unlink()
                    base.rmdir()

        self.session.owner.killer = killer
        self.assertEqual(self.session.disconnect(), "stopped")
        self.assertEqual([item[0] for item in original], [pid, pid])
        self.assertEqual(original[0][1], signal.SIGTERM)
        self.assertEqual(original[1][1], signal.SIGKILL)

    def test_close_stops_the_owned_call(self):
        self.session.connect()
        self.session.owner.killer = lambda p, sig: self.fake.killed.append((p, sig)) or self._drop(p, sig)
        self.assertEqual(self.session.close(), "stopped")
        self.assertIsNone(self.session.identity)

    def _drop(self, pid, sig):
        if sig == signal.SIGTERM:
            base = self.proc_root / str(pid)
            if base.exists():
                for child in list(base.iterdir()):
                    child.unlink()
                base.rmdir()

    def test_affinity_failure_does_not_leave_the_process(self):
        def boom(pid, cores):
            del pid, cores
            raise OSError("affinity")

        self.session.affinity = boom
        self.assertEqual(self.session.connect(), "error")
        self.assertEqual(self.session.process_text, "stopped")
        self.assertTrue(self.fake.killed)
        self.assertIsNone(self.session.identity)

    def test_stats_are_separate_from_process_start(self):
        self.session.connect()
        self.session.child = FakeChild(
            self.session.identity.pid,
            [
                'Bearer super-secret-token\n',
                '{"event":"stats","ice":"checking","ice_connected_ms":0,"video":{"codec":"video/H264","send_width":0,"send_height":0},"audio":{}}\n',
            ],
        )
        self.session.poll()
        self.assertEqual(self.session.media.transport(True), "stats, ice not connected")
        self.assertEqual(self.session.media.send_text(), "not measured")
        self.assertNotIn("super-secret-token", "\n".join(self.session.lines()))
        self.session.child = FakeChild(
            self.session.identity.pid,
            [
                '{"event":"stats","ice":"relay","ice_connected_ms":1200,"video":{"codec":"video/H264","send_width":1920,"send_height":1080,"send_fps":30,"recv_width":1280,"recv_height":720,"recv_fps":24},"audio":{"codec":"audio/opus","recv_packets":50}}\n',
                "remote window: openvr overlay IVROverlay_028\n",
            ],
        )
        self.session.poll()
        self.assertEqual(self.session.media.transport(True), "ice connected relay")
        self.assertEqual(self.session.media.send_text(), "video/H264 1920x1080 30.0 fps")
        self.assertEqual(self.session.media.recv_text(), "video/H264 1280x720 24.0 fps")
        self.assertEqual(self.session.media.audio_text(), "audio/opus")
        self.assertIn("openvr", self.session.media.remote_window)

    def test_thermal_needs_six_seconds_and_memory_is_a_ceiling(self):
        guard = ThermalGuard()
        self.assertFalse(guard.update(0, 85))
        self.assertFalse(guard.update(5, 85))
        self.assertTrue(guard.update(6, 85))
        dipped = ThermalGuard()
        dipped.update(0, 85)
        dipped.update(3, 79)
        self.assertFalse(dipped.update(6, 85))
        self.assertFalse(memory_tripped(7 * 1024 * 1024 * 1024 - 1))
        self.assertTrue(memory_tripped(7 * 1024 * 1024 * 1024))
        self.assertEqual(read_celsius("80000\n"), 80)
        self.assertEqual(read_rss_bytes("VmRSS:\t1024 kB\n"), 1024 * 1024)
        self.session.connect()
        self.session.child = FakeChild(self.session.identity.pid)
        self.session.poll("85000\n", "VmRSS:\t1000 kB\n")
        self.assertEqual(self.session.process_text, "running")
        self.clock.now = 6
        self.session.poll("85000\n", "VmRSS:\t1000 kB\n")
        self.assertIn("thermal", self.session.error)

    def test_fullscreen_call_image_shows_remote_video_and_hangup(self):
        from webrtcsays_panel.compose import compose
        from webrtcsays_panel.layout import REMOTE, remote_covers_most_of_the_view
        self.assertTrue(remote_covers_most_of_the_view())
        red = bytes([220, 20, 20, 255]) * 4
        pixels = compose(["WEBRTCSAYS.AI", "PROCESS: STOPPED"], red, (2, 2))
        x = REMOTE[0] + 100
        y = REMOTE[1] + 100
        i = (y * 1920 + x) * 4
        self.assertGreater(pixels[i], 180)
        self.assertLess(pixels[i + 1], 80)
        from webrtcsays_panel.layout import HANGUP
        hx = HANGUP[0] + 40
        hy = HANGUP[1] + 40
        j = (hy * 1920 + hx) * 4
        self.assertNotEqual(pixels[i:i + 3], pixels[j:j + 3])
        self.assertNotIn(b"super-secret-token", bytes(pixels))

    def test_overlay_click_hits_connect_and_not_the_remote_key(self):
        self.assertNotEqual(OVERLAY_KEY, REMOTE_VIDEO_OVERLAY_KEY)

        class Event:
            def __init__(self, x, y):
                self.eventType = MOUSE_DOWN
                self.data = self
                self.mouse = self
                self.x = x
                self.y = y

        from webrtcsays_panel.layout import CONNECT, HANGUP
        cx = CONNECT[0] + CONNECT[2] // 2
        cy = HEIGHT - (CONNECT[1] + CONNECT[3] // 2)
        hx = HANGUP[0] + HANGUP[2] // 2
        self.assertEqual(clicks_from_events([Event(cx, cy)]), ["connect"])
        self.assertEqual(clicks_from_events([Event(hx, cy)]), ["hangup"])
        self.assertEqual(hit_button(1, 1), None)
        self.assertEqual(self.session.connect(), "started")

    def test_redact_and_missing_files(self):
        self.assertEqual(redact("Authorization: Bearer secret"), "[redacted]")
        view = MediaView()
        view.ingest("token=super-secret-token")
        self.assertEqual(view.notes, ["[redacted]"])
        self.token.unlink()
        self.assertEqual(self.session.connect(), "error")
        self.assertEqual(self.spawned, [])

    def test_install_backs_up_only_this_desktop_entry(self):
        source = Path(__file__).resolve().parents[1]
        apps = self.home / ".local" / "share" / "applications"
        apps.mkdir(parents=True)
        other = apps / "Babelfish Live.desktop"
        other.write_text("Name=Babelfish Live\nExec=/bin/true\n", encoding="utf-8")
        old = apps / "WebRTCsays.AI.desktop"
        old.write_text("Name=old\n", encoding="utf-8")
        install_tree(source, self.home, "20261111")
        self.assertEqual(other.read_text(encoding="utf-8"), "Name=Babelfish Live\nExec=/bin/true\n")
        self.assertEqual(old.read_text(encoding="utf-8"), "Name=old\n")
        self.assertFalse((self.home / "babelfish" / "call" / "app" / "WebRTCsays.AI.desktop").exists())
        desktop = (source / "WebRTCsays.AI.desktop").read_text(encoding="utf-8")
        self.assertIn("Name=WebRTCsays.AI", desktop)
        self.assertNotIn("steamos-add-to-steam", desktop)
        self.assertNotIn("super-secret", desktop)
        script = (source / "webrtcsays_app.sh").read_text(encoding="utf-8")
        self.assertNotIn("pkill", script)
        panel = "\n".join(
            path.read_text(encoding="utf-8", errors="ignore")
            for path in (source / "webrtcsays_panel").rglob("*.py")
        )
        self.assertNotIn("pkill", panel)


if __name__ == "__main__":
    unittest.main()
