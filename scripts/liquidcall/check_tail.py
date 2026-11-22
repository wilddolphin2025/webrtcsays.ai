#!/usr/bin/env python3
"""Compare the C++ CPU tail with a NumPy inverse STFT of the same linear output."""

import struct
import sys

import numpy as np


def main():
    path = sys.argv[1]
    with open(path, "rb") as f:
        (n75,) = struct.unpack("<i", f.read(4))
        y = np.frombuffer(f.read(1282 * n75 * 4), dtype="<f4").reshape(1282, n75).copy()
        (npcm,) = struct.unpack("<i", f.read(4))
        pcm = np.frombuffer(f.read(npcm * 4), dtype="<f4").copy()
    n_fft, hop, nb = 1280, 320, 641
    window = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(n_fft) / n_fft)
    logmag, ang = np.split(y, 2, axis=0)
    spec = np.exp(logmag) * np.exp(1j * ang)
    frames = np.fft.irfft(spec, n=n_fft, axis=0) * window[:, None]
    out_len = (n75 - 1) * hop + n_fft
    acc = np.zeros(out_len, dtype=np.float64)
    env = np.zeros(out_len, dtype=np.float64)
    for t in range(n75):
        acc[t * hop : t * hop + n_fft] += frames[:, t]
        env[t * hop : t * hop + n_fft] += window ** 2
    trim = 480
    ref = acc[trim : out_len - trim] / np.maximum(env[trim : out_len - trim], 1e-8)
    if ref.shape[0] != pcm.shape[0]:
        print(f"length cpp={pcm.shape[0]} numpy={ref.shape[0]}")
        return 1
    err = np.max(np.abs(ref - pcm))
    print(f"tail max abs error {err:.3e} over {pcm.shape[0]} samples")
    if err > 1e-4:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
