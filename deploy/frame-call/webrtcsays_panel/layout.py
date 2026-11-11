"""Fullscreen call view. Origin is the top left of the 1920x1080 call image."""

WIDTH = 1920
HEIGHT = 1080
# Remote video fills the view above the controls.
REMOTE = (0, 0, 1920, 820)
PREVIEW = (1560, 24, 328, 184)
CONNECT = (32, 860, 900, 188)
HANGUP = (988, 860, 900, 188)
BUTTONS = {"connect": CONNECT, "hangup": HANGUP}

# Head-locked surface. 2.6 m wide at 1.05 m is about 102 degrees across.
SURFACE_WIDTH_METERS = 2.6
SURFACE_DISTANCE_METERS = 1.05
# Not an eye-buffer Submit. The far periphery of the headset can remain outside this image.
PRESENTER = "fov-call-surface"


def hit_button(px, py):
    for name, (x, y, w, h) in BUTTONS.items():
        if x <= px < x + w and y <= py < y + h:
            return name
    return None


def remote_covers_most_of_the_view():
    _, _, w, h = REMOTE
    return w * h > (WIDTH * HEIGHT) // 2
