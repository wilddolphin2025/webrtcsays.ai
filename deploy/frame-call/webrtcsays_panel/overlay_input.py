"""Map OpenVR overlay mouse events onto the panel. Y is flipped from the bottom-left origin."""

from webrtcsays_panel.layout import HEIGHT, hit_button

MOUSE_DOWN = 301


def clicks_from_events(events):
    hits = []
    for event in events:
        if getattr(event, "eventType", None) != MOUSE_DOWN:
            continue
        mouse = event.data.mouse
        px = float(mouse.x)
        py = HEIGHT - float(mouse.y)
        name = hit_button(px, py)
        if name:
            hits.append(name)
    return hits
