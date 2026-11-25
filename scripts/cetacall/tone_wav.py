#!/usr/bin/env python3
"""Write a short 24 kHz mono PCM tone so a local call needs no external clip."""
import math
import struct
import sys
import wave

path = sys.argv[1]
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0
rate = 24000
count = int(rate * seconds)
with wave.open(path, "w") as handle:
    handle.setnchannels(1)
    handle.setsampwidth(2)
    handle.setframerate(rate)
    frames = bytearray()
    for i in range(count):
        sample = int(0.2 * 32767 * math.sin(2 * math.pi * 440 * i / rate))
        frames += struct.pack("<h", sample)
    handle.writeframes(frames)
print(path)
