"""Pixel layout for the headset panel. Origin is the top left."""

WIDTH = 1280
HEIGHT = 720

# The button band is vertically centered so a top/bottom mouse-origin
# mistake still lands on the same controls.
CONNECT = (48, 290, 560, 140)
DISCONNECT = (672, 290, 560, 140)
BUTTONS = {"connect": CONNECT, "disconnect": DISCONNECT}


def hit_button(px, py):
    for name, (x, y, w, h) in BUTTONS.items():
        if x <= px < x + w and y <= py < y + h:
            return name
    return None
