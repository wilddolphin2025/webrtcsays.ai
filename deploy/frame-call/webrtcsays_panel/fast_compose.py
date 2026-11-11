"""Place remote frames into one reused 1920x1080 canvas.

The video rectangle is copied when the native writer already scaled to it.
A size change uses precomputed integer maps. The button image is rebuilt
only when the status text changes.
"""

import numpy as np

from webrtcsays_panel.compose import compose
from webrtcsays_panel.layout import HEIGHT, REMOTE, WIDTH


class CallCanvas:
    def __init__(self):
        self.pixels = np.empty((HEIGHT, WIDTH, 4), dtype=np.uint8)
        self.ui_key = None
        self._ys = None
        self._xs = None
        self._map_key = None

    def set_status(self, lines):
        key = tuple(lines)
        if key == self.ui_key:
            return False
        raw = compose(list(lines))
        drawn = np.frombuffer(raw, dtype=np.uint8).reshape(HEIGHT, WIDTH, 4)
        self.pixels[:, :, :] = drawn
        self.ui_key = key
        return True

    def paint_video(self, src):
        """src is a uint8 array shaped (height, width, 4)."""
        sh, sw = int(src.shape[0]), int(src.shape[1])
        _x, _y, dw, dh = REMOTE
        slot = self.pixels[0:dh, 0:dw]
        if sh == dh and sw == dw:
            slot[:, :, :] = src
            return "copy"
        key = (sh, sw, dh, dw)
        if key != self._map_key:
            self._ys = (np.arange(dh, dtype=np.int32) * sh) // dh
            self._xs = (np.arange(dw, dtype=np.int32) * sw) // dw
            self._map_key = key
        slot[:, :, :] = src[self._ys][:, self._xs]
        return "scale"
