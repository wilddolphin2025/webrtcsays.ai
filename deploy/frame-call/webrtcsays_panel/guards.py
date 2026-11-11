"""Stop the owned call on a sustained 80C reading or a 7 GiB RSS."""

from webrtcsays_panel.command import MEMORY_LIMIT_BYTES, THERMAL_LIMIT_C, THERMAL_SUSTAIN_SEC


class ThermalGuard:
    def __init__(self, limit_c=THERMAL_LIMIT_C, sustain_s=THERMAL_SUSTAIN_SEC):
        self.limit_c = limit_c
        self.sustain_s = sustain_s
        self.samples = []

    def update(self, now, celsius):
        if celsius is None:
            return False
        self.samples.append((float(now), float(celsius)))
        horizon = float(now) - self.sustain_s
        self.samples = [(t, c) for t, c in self.samples if t >= horizon - 1.0]
        window = [(t, c) for t, c in self.samples if t >= horizon]
        if len(window) < 2:
            return False
        span = window[-1][0] - window[0][0]
        if span + 1e-6 < self.sustain_s:
            return False
        return all(c >= self.limit_c for _, c in window)


def memory_tripped(rss_bytes, limit=MEMORY_LIMIT_BYTES):
    return rss_bytes is not None and rss_bytes >= limit


def hottest_celsius(texts):
    """Use the hottest available zone. zone0 alone can stay near 40C."""
    values = []
    for text in texts or []:
        value = read_celsius(text)
        if value is not None:
            values.append(value)
    if not values:
        return None
    return max(values)


def read_celsius(text):
    """Parse a sysfs thermal value. Millidegree files are above 1000."""
    if text is None:
        return None
    try:
        value = float(str(text).strip())
    except ValueError:
        return None
    if value >= 1000:
        value = value / 1000.0
    return value


def read_rss_bytes(status_text):
    if not status_text:
        return None
    for line in status_text.splitlines():
        if line.startswith("VmRSS:"):
            parts = line.split()
            return int(parts[1]) * 1024
    return None
