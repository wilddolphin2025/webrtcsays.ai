"""Place remote frames into one reused 1920x1080 canvas.

A 16:9 frame fills the plane. Any other ratio is centered on opaque bars.
The button image is rebuilt only when the status text changes, then copied
back over each frame.
"""

import numpy as np

from webrtcsays_panel.compose import compose
from webrtcsays_panel.layout import BAR, HEIGHT, PLATE, PREVIEW, WIDTH, fit_frame


class CallCanvas:
    def __init__(self):
        self.pixels = np.empty((HEIGHT, WIDTH, 4), dtype=np.uint8)
        self.pixels[:, :] = BAR
        self.ui_key = None
        self._controls = None
        self._ys = None
        self._xs = None
        self._map_key = None

    def set_status(self, lines):
        key = tuple(lines)
        if key == self.ui_key:
            return False
        raw = compose(list(lines))
        drawn = np.frombuffer(raw, dtype=np.uint8).reshape(HEIGHT, WIDTH, 4)
        self._controls = drawn.copy()
        self.pixels[:, :, :] = self._controls
        self.ui_key = key
        return True

    def _restore_controls(self):
        if self._controls is None:
            return
        for x, y, w, h in (PLATE, PREVIEW):
            self.pixels[y:y + h, x:x + w] = self._controls[y:y + h, x:x + w]

    def paint_video(self, src):
        """src is a uint8 array shaped (height, width, 4). Aspect is kept."""
        sh, sw = int(src.shape[0]), int(src.shape[1])
        x, y, dw, dh = fit_frame(sw, sh)
        full = x == 0 and y == 0 and dw == WIDTH and dh == HEIGHT
        if not full:
            self.pixels[:, :] = BAR
        if dw > 0 and dh > 0:
            if sh == dh and sw == dw:
                self.pixels[y:y + dh, x:x + dw] = src
                mode = "copy"
            elif dh % sh == 0 and dw % sw == 0:
                scaled = np.repeat(np.repeat(src, dh // sh, axis=0), dw // sw, axis=1)
                self.pixels[y:y + dh, x:x + dw] = scaled
                mode = "scale"
            else:
                key = (sh, sw, dh, dw)
                if key != self._map_key:
                    self._ys = (np.arange(dh, dtype=np.int32) * sh) // dh
                    self._xs = (np.arange(dw, dtype=np.int32) * sw) // dw
                    self._map_key = key
                self.pixels[y:y + dh, x:x + dw] = src[self._ys][:, self._xs]
                mode = "scale"
            self.pixels[y:y + dh, x:x + dw, 3] = 255
        else:
            mode = "empty"
        self._restore_controls()
        return mode
