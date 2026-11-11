"""Start and stop one owned directcall. Reconnect does not spawn a second copy."""

import os
import signal
from pathlib import Path

from webrtcsays_panel.command import CORES, DEFAULT_DURATION_SEC, directcall_argv
from webrtcsays_panel.guards import ThermalGuard, memory_tripped, read_celsius, read_rss_bytes
from webrtcsays_panel.owner import Owner
from webrtcsays_panel.stats import MediaView


class Session:
    def __init__(self, paths, proc, popen, affinity, killer, clock, sleeper, duration_sec=DEFAULT_DURATION_SEC):
        self.paths = paths
        self.proc = proc
        self.popen = popen
        self.affinity = affinity
        self.owner = Owner(proc, killer)
        self.clock = clock
        self.sleeper = sleeper
        self.duration_sec = duration_sec
        self.identity = None
        self.child = None
        self.media = MediaView()
        self.thermal = ThermalGuard()
        self.error = ""
        self.guard = ""
        self.process_text = "stopped"
        self._lines = []

    def argv(self):
        return directcall_argv(
            self.paths["binary"], self.paths["image"], self.paths["token"], self.duration_sec
        )

    def preflight(self):
        binary = Path(self.paths["binary"])
        image = Path(self.paths["image"])
        token = Path(self.paths["token"])
        if not binary.is_file() or not os.access(binary, os.X_OK):
            return "directcall binary is missing or not executable"
        if not image.is_file():
            return "still image is missing"
        if not token.is_file():
            return "token file is missing"
        return ""

    def connect(self):
        if self._verified():
            self.error = ""
            self.process_text = "running"
            return "already-running"
        problem = self.preflight()
        if problem:
            self.error = problem
            return "error"
        try:
            child = self.popen(self.argv())
        except OSError as exc:
            self.error = "failed to start directcall"
            self._note(str(exc))
            return "error"
        self.child = child
        try:
            self.affinity(child.pid, set(CORES))
        except OSError:
            self._stop_child(child)
            self.error = "could not pin cores 2,3,4,7"
            self.process_text = "stopped"
            return "error"
        identity = self.proc.identity(child.pid)
        expected_exe = str(Path(self.paths["binary"]).resolve())
        if identity is None or identity.exe != expected_exe:
            self._stop_child(child)
            self.error = "started process did not stay the directcall binary"
            self.process_text = "stopped"
            return "error"
        if identity.cmdline != tuple(self.argv()):
            self._stop_child(child)
            self.error = "started process command does not match the requested call"
            self.process_text = "stopped"
            return "error"
        self.identity = identity
        self.error = ""
        self.guard = ""
        self.process_text = "running"
        self.media = MediaView()
        return "started"

    def disconnect(self):
        if self.identity is None:
            self.process_text = "stopped"
            return "idle"
        result = self._signal_and_wait(signal.SIGTERM, 5.0)
        if result == "refused":
            self.error = "refusing to signal, process identity changed"
            self.identity = None
            self.process_text = "stopped"
            return "refused"
        if self._verified():
            killed = self._signal_and_wait(signal.SIGKILL, 2.0)
            if killed == "refused":
                self.error = "refusing to signal, process identity changed"
                self.identity = None
                self.process_text = "stopped"
                return "refused"
        self.identity = None
        self.child = None
        self.process_text = "stopped"
        return "stopped"

    def close(self):
        return self.disconnect()

    def poll(self, thermal_text=None, status_text=None):
        self._drain()
        running = self._verified()
        self.process_text = "running" if running else "stopped"
        if self.identity is not None and not running:
            self.identity = None
            self.child = None
            if not self.error:
                self.error = "call process exited"
        now = self.clock()
        if self.thermal.update(now, read_celsius(thermal_text)):
            self.guard = "thermal %.0fC sustained %.0fs" % (self.thermal.limit_c, self.thermal.sustain_s)
            self.error = self.guard
            if running:
                self.disconnect()
            return
        rss = read_rss_bytes(status_text)
        if running and memory_tripped(rss):
            self.guard = "memory guard"
            self.error = "memory reached 7GiB"
            self.disconnect()

    def lines(self):
        return list(self._lines)

    def _verified(self):
        return self.identity is not None and self.proc.alive(self.identity)

    def _drain(self):
        if self.child is None:
            return
        for line in self.child.drain():
            self.media.ingest(line)
            shown = self.media.notes[-1] if self.media.notes else ""
            if shown and (not self._lines or self._lines[-1] != shown):
                self._lines.append(shown)
                del self._lines[:-12]

    def _note(self, text):
        self.media.ingest(text)

    def _stop_child(self, child):
        identity = self.proc.identity(child.pid)
        if identity is None:
            return
        self.owner.signal(identity, signal.SIGKILL)

    def _signal_and_wait(self, sig, seconds):
        result = self.owner.signal(self.identity, sig)
        if result != "signaled":
            return result
        # A stuck sleeper cannot hang the panel. The step cap is the wait.
        for _ in range(int(seconds * 10) + 1):
            if not self._verified():
                return "stopped"
            self.sleeper(0.1)
        return "timeout"
