"""Upload when the visible image changes. There is no frames-per-second cap.

Idle stays on the first upload. A new remote frame index is a new image
and is uploaded on the next pass, including at 30 new frames per second.
"""


class UploadBudget:
    def __init__(self):
        self.signature = None
        self.uploads = 0

    def should_upload(self, signature, now=None):
        del now
        if signature == self.signature:
            return False
        self.signature = signature
        self.uploads += 1
        return True
