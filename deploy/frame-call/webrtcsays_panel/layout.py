"""Fullscreen call view. Origin is the top left of the 1920x1080 call image."""

WIDTH = 1920
HEIGHT = 1080
# Remote video fills the view above the controls. The fast copy uses this
# rectangle from the origin, so its size stays 1920x820.
REMOTE = (0, 0, 1920, 820)
PREVIEW = (1560, 24, 328, 184)
# Head-locked surface. 2.6 m wide at 1.05 m is about 102 degrees across.
# A control at the left edge is about 51 degrees off the nose. The buttons
# and status sit together near the middle, about 14 degrees either side.
FOV_DEGREES = 102.0
CLUSTER_W = 1008
CLUSTER_X = (WIDTH - CLUSTER_W) // 2
GAP = 48
BTN_W = 480
BTN_H = 168
BTN_Y = 892
CONNECT = (CLUSTER_X, BTN_Y, BTN_W, BTN_H)
HANGUP = (CLUSTER_X + BTN_W + GAP, BTN_Y, BTN_W, BTN_H)
STATUS = (CLUSTER_X, 824, CLUSTER_W, 60)
BUTTONS = {"connect": CONNECT, "hangup": HANGUP}
SURFACE_WIDTH_METERS = 2.6
SURFACE_DISTANCE_METERS = 1.05
# Not an eye-buffer Submit. The far periphery of the headset can remain outside this image.
PRESENTER = "fov-call-surface"


def rect_center(rect):
    x, y, w, h = rect
    return x + w / 2.0, y + h / 2.0


def horizontal_degrees(px):
    return (float(px) / WIDTH - 0.5) * FOV_DEGREES


def overlaps(a, b):
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    return ax < bx + bw and bx < ax + aw and ay < by + bh and by < ay + ah


def hit_button(px, py):
    for name, (x, y, w, h) in BUTTONS.items():
        if x <= px < x + w and y <= py < y + h:
            return name
    return None


def remote_covers_most_of_the_view():
    _, _, w, h = REMOTE
    return w * h > (WIDTH * HEIGHT) // 2
