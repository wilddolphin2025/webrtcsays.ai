"""Run tests/test_pages.js under node so `python3 -m unittest discover` covers the pages."""
from __future__ import annotations

import shutil
import subprocess
import unittest
from pathlib import Path


class PageScriptTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("node"), "node not installed")
    def test_pages_js(self) -> None:
        script = Path(__file__).with_name("test_pages.js")
        proc = subprocess.run(["node", str(script)], capture_output=True, text=True, timeout=60)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("all page tests passed", proc.stdout)


if __name__ == "__main__":
    unittest.main()
