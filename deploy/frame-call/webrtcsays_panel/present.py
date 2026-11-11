"""Upload the call image only when it changes, and not faster than 4 Hz.

The Frame idle run redrew a 1920x1080 image every 100 ms. RSS rose from
49 MB to 972 MB in 80 s, about 11 MB/s, mostly shared. One core stayed
at 85-100%. Repeating SetOverlayRaw is the churn. An unchanged idle
image is uploaded once.
"""

MIN_UPLOAD_INTERVAL_SEC = 0.25


class UploadBudget:
    def __init__(self, min_interval=MIN_UPLOAD_INTERVAL_SEC):
        self.min_interval = min_interval
        self.signature = None
        self.pending = None
        self.last_upload = None
        self.uploads = 0
        self.composes = 0

    def should_upload(self, signature, now):
        if signature != self.signature:
            self.pending = signature
        elif self.pending is None:
            return False
        if self.last_upload is not None and (now - self.last_upload) < self.min_interval:
            return False
        self.signature = self.pending if self.pending is not None else signature
        self.pending = None
        self.last_upload = now
        self.uploads += 1
        self.composes += 1
        return True
