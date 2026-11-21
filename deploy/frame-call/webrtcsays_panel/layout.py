"""Fullscreen call view. Origin is the top left of the 1920x1080 call image."""

WIDTH = 1920
HEIGHT = 1080
# The video plane is the whole overlay. Frames are fitted inside it.
REMOTE = (0, 0, WIDTH, HEIGHT)
PREVIEW = (1656, 24, 240, 135)
# Head-locked surface. 2.6 m wide at 1.05 m is about 102 degrees across.
FOV_DEGREES = 102.0
CONNECT = (646, 808, 300, 84)
HANGUP = (974, 808, 300, 84)
STATUS = (646, 762, 628, 36)
# Opaque plate behind the cluster, 14 px outside the status line and buttons.
PLATE = (632, 748, 656, 158)
BUTTONS = {"connect": CONNECT, "hangup": HANGUP}
# Opaque video bars. A fitted frame must not leave alpha at 0.
BAR = (8, 12, 18, 255)
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


def fit_frame(src_w, src_h, plane=None):
    """Uniform scale of src into plane. 16:9 fills 1920x1080. Other ratios are centered."""
    if plane is None:
        plane = REMOTE
    px, py, pw, ph = plane
    src_w = int(src_w)
    src_h = int(src_h)
    if src_w <= 0 or src_h <= 0 or pw <= 0 or ph <= 0:
        return (px, py, 0, 0)
    if src_w * ph <= src_h * pw:
        dh = ph
        dw = src_w * ph // src_h
    else:
        dw = pw
        dh = src_h * pw // src_w
    dw = min(pw, max(1, dw))
    dh = min(ph, max(1, dh))
    x = px + (pw - dw) // 2
    y = py + (ph - dh) // 2
    return (x, y, dw, dh)


def remote_covers_most_of_the_view():
    _, _, w, h = REMOTE
    return w * h > (WIDTH * HEIGHT) // 2
