#!/usr/bin/env python3
"""Browser send-path checks. No production host and no real camera."""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
CHROME = os.environ.get("CHROME_BIN", "/usr/local/bin/google-chrome")
NODE = os.environ.get("NODE_BIN", "node")


class BrowserSendTest(unittest.TestCase):
    def test_share_state_machine(self) -> None:
        script = ROOT / "tests" / "test_media_state.js"
        done = subprocess.run([NODE, str(script)], check=False, capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn("media-state ok", done.stdout)

    def test_late_camera_and_mic_send_frames(self) -> None:
        if not pathlib.Path(CHROME).is_file():
            self.skipTest("chrome is not installed")
        env = os.environ.copy()
        env["NODE_PATH"] = "/tmp/probe-deps/node_modules"
        env["CHROME_BIN"] = CHROME
        done = subprocess.run(
            [NODE, str(ROOT / "tests" / "run_send_probe.js")],
            check=False,
            capture_output=True,
            text=True,
            timeout=40,
            env=env,
        )
        text = done.stdout + "\n" + done.stderr
        self.assertEqual(done.returncode, 0, text[-4000:])
        self.assertIn("RESULT ", text, text[-2000:])
        raw = text.split("RESULT ", 1)[1].splitlines()[0]
        result = json.loads(raw)
        self.assertTrue(result.get("ok"), result)
        self.assertEqual(result.get("videoDir"), "sendrecv")
        self.assertEqual(result.get("audioDir"), "sendrecv")
        self.assertGreater(result.get("frames", 0), 0)
        self.assertGreaterEqual(result.get("width", 0), 160)
        self.assertGreater(result.get("videoBytes", 0), 0)
        self.assertGreater(result.get("audioBytes", 0), 0)


if __name__ == "__main__":
    unittest.main()
