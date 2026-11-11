"""Draw the fullscreen call image: remote video, local preview, Connect, Hangup, status."""

import struct
import zlib

from webrtcsays_panel.layout import (
    BUTTONS,
    CONNECT,
    HANGUP,
    HEIGHT,
    PREVIEW,
    REMOTE,
    WIDTH,
)

# 5x7 glyphs. Each row is five bits, MSB on the left.
_GLYPHS = {
    " ": [0, 0, 0, 0, 0, 0, 0],
    "A": [14, 17, 17, 31, 17, 17, 17],
    "B": [30, 17, 17, 30, 17, 17, 30],
    "C": [14, 17, 16, 16, 16, 17, 14],
    "D": [30, 17, 17, 17, 17, 17, 30],
    "E": [31, 16, 16, 30, 16, 16, 31],
    "F": [31, 16, 16, 30, 16, 16, 16],
    "G": [14, 17, 16, 23, 17, 17, 14],
    "H": [17, 17, 17, 31, 17, 17, 17],
    "I": [14, 4, 4, 4, 4, 4, 14],
    "L": [16, 16, 16, 16, 16, 16, 31],
    "M": [17, 27, 21, 21, 17, 17, 17],
    "N": [17, 25, 21, 19, 17, 17, 17],
    "O": [14, 17, 17, 17, 17, 17, 14],
    "P": [30, 17, 17, 30, 16, 16, 16],
    "R": [30, 17, 17, 30, 20, 18, 17],
    "S": [14, 17, 16, 14, 1, 17, 14],
    "T": [31, 4, 4, 4, 4, 4, 4],
    "U": [17, 17, 17, 17, 17, 17, 14],
    "V": [17, 17, 17, 17, 17, 10, 4],
    "W": [17, 17, 17, 21, 21, 21, 10],
    "X": [17, 17, 10, 4, 10, 17, 17],
    "Y": [17, 17, 10, 4, 4, 4, 4],
    "0": [14, 17, 19, 21, 25, 17, 14],
    "1": [4, 12, 4, 4, 4, 4, 14],
    "2": [14, 17, 1, 6, 8, 16, 31],
    "3": [30, 1, 1, 14, 1, 1, 30],
    "4": [2, 6, 10, 18, 31, 2, 2],
    "5": [31, 16, 30, 1, 1, 17, 14],
    "6": [14, 16, 16, 30, 17, 17, 14],
    "7": [31, 1, 2, 4, 8, 8, 8],
    "8": [14, 17, 17, 14, 17, 17, 14],
    "9": [14, 17, 17, 15, 1, 1, 14],
    ":": [0, 4, 4, 0, 4, 4, 0],
    ".": [0, 0, 0, 0, 0, 12, 12],
    "x": [0, 17, 10, 4, 10, 17, 0],
    "-": [0, 0, 0, 31, 0, 0, 0],
    "/": [1, 2, 4, 8, 16, 0, 0],
}


def _blank():
    pixels = bytearray(WIDTH * HEIGHT * 4)
    for i in range(0, len(pixels), 4):
        pixels[i] = 27
        pixels[i + 1] = 40
        pixels[i + 2] = 56
        pixels[i + 3] = 255
    return pixels


def _fill(pixels, rect, color):
    x0, y0, w, h = rect
    r, g, b = color
    for y in range(y0, min(HEIGHT, y0 + h)):
        row = y * WIDTH
        for x in range(x0, min(WIDTH, x0 + w)):
            i = (row + x) * 4
            pixels[i] = r
            pixels[i + 1] = g
            pixels[i + 2] = b
            pixels[i + 3] = 255


def _blit(pixels, rect, src, src_w, src_h):
    if src is None or src_w <= 0 or src_h <= 0:
        return
    x0, y0, w, h = rect
    for y in range(h):
        sy = y * src_h // h
        dy = y0 + y
        if dy < 0 or dy >= HEIGHT:
            continue
        for x in range(w):
            dx = x0 + x
            if dx < 0 or dx >= WIDTH:
                continue
            sx = x * src_w // w
            s = (sy * src_w + sx) * 4
            d = (dy * WIDTH + dx) * 4
            pixels[d:d + 4] = src[s:s + 4]


def _text(pixels, x, y, text, scale, color):
    cursor = x
    r, g, b = color
    for ch in text.upper():
        rows = _GLYPHS.get(ch, _GLYPHS[" "])
        for gy, bits in enumerate(rows):
            for gx in range(5):
                if bits & (1 << (4 - gx)):
                    _fill(pixels, (cursor + gx * scale, y + gy * scale, scale, scale), (r, g, b))
        cursor += 6 * scale


def compose(lines, remote=None, remote_size=None, preview=None, preview_size=None):
    pixels = _blank()
    _fill(pixels, REMOTE, (8, 12, 18))
    if remote and remote_size:
        _blit(pixels, REMOTE, remote, remote_size[0], remote_size[1])
    else:
        _text(pixels, 48, 360, "REMOTE VIDEO NOT MEASURED", 4, (199, 213, 224))
    _fill(pixels, PREVIEW, (14, 20, 27))
    if preview and preview_size:
        _blit(pixels, PREVIEW, preview, preview_size[0], preview_size[1])
    else:
        _text(pixels, PREVIEW[0] + 16, PREVIEW[1] + 70, "LOCAL STILL", 3, (199, 213, 224))
    _fill(pixels, CONNECT, (26, 159, 255))
    _fill(pixels, HANGUP, (42, 71, 94))
    _text(pixels, CONNECT[0] + 250, CONNECT[1] + 70, "CONNECT", 6, (14, 20, 27))
    _text(pixels, HANGUP[0] + 280, HANGUP[1] + 70, "HANGUP", 6, (199, 213, 224))
    y = 790
    for line in lines[:3]:
        _text(pixels, 36, y, line, 2, (214, 217, 220))
        y += 22
    return pixels


def write_png(path, pixels, width=WIDTH, height=HEIGHT):
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        row = y * width * 4
        for x in range(width):
            i = row + x * 4
            raw.extend(pixels[i:i + 3])
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw), 1)) + chunk(b"IEND", b"")
    with open(path, "wb") as handle:
        handle.write(png)


def read_remote_frame(blob):
    if blob is None or len(blob) < 16:
        return None
    magic, width, height, index = struct.unpack_from("<IIII", blob, 0)
    if magic != 0x314D5246 or width <= 0 or height <= 0 or width > 4096 or height > 4096:
        return None
    need = 16 + width * height * 4
    if len(blob) < need:
        return None
    return width, height, index, blob[16:need]
