"""Own one directcall process by pid, start time, exe, and exact argv."""

import os
from pathlib import Path


class ProcessIdentity:
    def __init__(self, pid, start_ticks, exe, cmdline):
        self.pid = int(pid)
        self.start_ticks = int(start_ticks)
        self.exe = str(exe)
        self.cmdline = tuple(cmdline)

    def same_process(self, other):
        return (
            other is not None
            and self.pid == other.pid
            and self.start_ticks == other.start_ticks
            and self.exe == other.exe
            and self.cmdline == other.cmdline
        )


def _start_ticks(stat_text):
    end = stat_text.rfind(")")
    if end < 0:
        raise ValueError("proc stat has no command field")
    fields = stat_text[end + 2 :].split()
    # Field 22 is starttime. Fields after comm begin at field 3.
    return int(fields[19])


class ProcFs:
    def __init__(self, root="/proc"):
        self.root = Path(root)

    def identity(self, pid):
        base = self.root / str(pid)
        try:
            stat_text = (base / "stat").read_text(encoding="utf-8")
            cmdline = (base / "cmdline").read_bytes().split(b"\0")
            exe = os.readlink(base / "exe")
        except (OSError, ValueError, IndexError):
            return None
        args = tuple(part.decode("utf-8", "replace") for part in cmdline if part)
        if not args:
            return None
        return ProcessIdentity(pid, _start_ticks(stat_text), exe, args)

    def alive(self, identity):
        return identity.same_process(self.identity(identity.pid))


class Owner:
    """Signal only a process whose identity still matches the one we stored."""

    def __init__(self, proc, killer):
        self.proc = proc
        self.killer = killer

    def signal(self, identity, sig):
        current = self.proc.identity(identity.pid)
        if current is None:
            return "gone"
        if not identity.same_process(current):
            return "refused"
        self.killer(identity.pid, sig)
        return "signaled"
