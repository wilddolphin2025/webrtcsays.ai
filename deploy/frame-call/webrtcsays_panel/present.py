"""Upload when the visible image changes. There is no frames-per-second cap.

Idle stays on the first upload. A new remote frame index is a new image
and is uploaded on the next pass, including at 30 new frames per second.
"""


def pixel_nbytes(pixels):
    """Byte length of an image. A numpy frame's len() is its row count, not its size."""
    nbytes = getattr(pixels, "nbytes", None)
    if isinstance(nbytes, int) and nbytes > 0:
        return nbytes
    return len(pixels)


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
