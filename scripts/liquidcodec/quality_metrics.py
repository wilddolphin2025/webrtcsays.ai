#!/usr/bin/env python3
"""Quality scores for the LiquidCodec ladder.

Time alignment is a cross-correlation lag. SNR and segmental SNR are on the
overlap after that shift. They are not gain-normalized: a quieter decode
scores lower. Log-spectral distance is the mean RMS of 20*log10 magnitude
spectra. PESQ-WB is optional (pip install pesq) and is skipped when the
package is absent. Higher SNR is better. Lower LSD is better.
"""

import argparse
import csv
import math
import wave
from pathlib import Path

try:
    import numpy as np
except ImportError:
    np = None


def read_wav(path):
    with wave.open(str(path), "rb") as wf:
        channels = wf.getnchannels()
        rate = wf.getframerate()
        width = wf.getsampwidth()
        n = wf.getnframes()
        raw = wf.readframes(n)
    if width != 2:
        raise SystemExit(f"{path} is not 16-bit PCM")
    samples = np.frombuffer(raw, dtype=np.int16).astype(np.float64)
    if channels > 1:
        samples = samples.reshape(-1, channels).mean(axis=1)
    return rate, samples


def write_wav(path, rate, samples):
    pcm = np.clip(np.round(samples), -32768, 32767).astype(np.int16)
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(rate)
        wf.writeframes(pcm.tobytes())


def generate_clips(dest, quick):
    """Three generated clips. No recorded speech, so there is nothing to license."""
    if np is None:
        raise SystemExit("numpy is required to generate clips")
    dest = Path(dest)
    dest.mkdir(parents=True, exist_ok=True)
    rate = 24000
    seconds = 0.8 if quick else 2.0
    n = int(rate * seconds)
    t = np.arange(n) / rate
    # Vowel: harmonics under a couple of formant bumps.
    vowel = np.zeros(n)
    for h in range(1, 12):
        vowel += (1.0 / h) * np.sin(2 * math.pi * 110 * h * t)
    env = np.exp(-((np.linspace(-1, 1, n)) ** 2) * 2)
    vowel *= 0.25 * env * 8000
    # Noise bursts, a stand-in for a sibilant.
    rng = np.random.default_rng(1)
    noise = rng.standard_normal(n) * 2000
    gate = (np.sin(2 * math.pi * 4 * t) > 0.4).astype(np.float64)
    noise *= gate
    # Glide: a chirp plus a quieter vowel.
    glide = 6000 * np.sin(2 * math.pi * (200 * t + 400 * t * t)) * env
    glide += 0.15 * vowel
    paths = []
    for name, data in (("vowel", vowel), ("noise", noise), ("glide", glide)):
        path = dest / f"{name}.wav"
        write_wav(path, rate, data)
        paths.append(path)
    return paths


def shift(sig, lag):
    if lag > 0:
        return np.concatenate([np.zeros(lag), sig])
    if lag < 0:
        return sig[-lag:]
    return sig


def choose_lag(ref, deg, max_lag):
    a = ref - np.mean(ref)
    b = deg - np.mean(deg)
    n = 1 << int(math.ceil(math.log2(len(a) + len(b) + 1)))
    corr = np.fft.irfft(np.fft.rfft(a, n) * np.conj(np.fft.rfft(b, n)), n)
    # corr[k] matches ref with deg delayed by k (deg[i] ~= ref[i - k]).
    window = np.concatenate([corr[: max_lag + 1], corr[n - max_lag :]])
    idx = int(np.argmax(window))
    if idx <= max_lag:
        raw = idx
    else:
        raw = idx - (2 * max_lag + 1)
    # A clip delayed by inserting zeros peaks at a negative raw lag.
    # Positive means the degraded signal is late.
    return -raw


def snr_db(ref, deg):
    n = min(len(ref), len(deg))
    if n == 0:
        return 0.0
    err = ref[:n] - deg[:n]
    sig = float(np.dot(ref[:n], ref[:n]))
    noise = float(np.dot(err, err))
    if noise <= 1e-9:
        return 100.0
    if sig <= 1e-9:
        return 0.0
    return 10.0 * math.log10(sig / noise)


def seg_snr_db(ref, deg, rate):
    frame = max(1, int(rate * 0.02))
    n = min(len(ref), len(deg))
    scores = []
    for i in range(0, n - frame + 1, frame):
        chunk_r = ref[i : i + frame]
        if float(np.dot(chunk_r, chunk_r)) < 1e6:
            continue
        scores.append(min(80.0, snr_db(chunk_r, deg[i : i + frame])))
    if not scores:
        return snr_db(ref[:n], deg[:n])
    return float(np.mean(scores))


def lsd_db(ref, deg, rate):
    nfft = 512
    hop = 256
    window = np.hanning(nfft)
    m = min(len(ref), len(deg))
    vals = []
    for i in range(0, m - nfft, hop):
        spec_r = np.abs(np.fft.rfft(ref[i : i + nfft] * window)) + 1e-9
        spec_d = np.abs(np.fft.rfft(deg[i : i + nfft] * window)) + 1e-9
        diff = 20.0 * np.log10(spec_r) - 20.0 * np.log10(spec_d)
        vals.append(float(np.sqrt(np.mean(diff * diff))))
    return float(np.mean(vals)) if vals else 0.0


def pesq_wb(ref, deg, rate):
    try:
        from pesq import pesq
    except ImportError:
        return None
    # PESQ wideband is 16 kHz. Resample with a 10 ms average if needed.
    def to_16k(sig, src):
        if src == 16000:
            return sig
        if src % 16000 == 0:
            step = src // 16000
            return sig[::step]
        # Linear interpolation is enough for an optional score.
        out_n = int(len(sig) * 16000 / src)
        x = np.linspace(0, len(sig) - 1, out_n)
        i0 = np.floor(x).astype(int)
        i1 = np.minimum(i0 + 1, len(sig) - 1)
        frac = x - i0
        return sig[i0] * (1 - frac) + sig[i1] * frac

    try:
        return float(pesq(16000, to_16k(ref, rate), to_16k(deg, rate), "wb"))
    except Exception:
        return None


def score_pair(ref_path, deg_path, max_lag_ms=500):
    if np is None:
        raise SystemExit("numpy is required to score wavs")
    rate_r, ref = read_wav(ref_path)
    rate_d, deg = read_wav(deg_path)
    if rate_r != rate_d:
        # Score at the reference rate.
        if rate_d % 100 != 0 or rate_r % 100 != 0:
            raise SystemExit("sample rates must be multiples of 100")
        # Reuse the C++ 10 ms rule in pure numpy: repeat-or-drop is too crude
        # for a quality number, so resample linearly.
        out_n = int(len(deg) * rate_r / rate_d)
        x = np.linspace(0, len(deg) - 1, out_n)
        i0 = np.floor(x).astype(int)
        i1 = np.minimum(i0 + 1, len(deg) - 1)
        deg = deg[i0] * (1 - (x - i0)) + deg[i1] * (x - i0)
        rate_d = rate_r
    max_lag = int(rate_r * max_lag_ms / 1000)
    lag = choose_lag(ref, deg, max_lag)
    # Positive lag means the degraded signal is late. Advance it.
    aligned = shift(deg, -lag)
    n = min(len(ref), len(aligned))
    ref_n = ref[:n]
    deg_n = aligned[:n]
    snr = snr_db(ref_n, deg_n)
    seg = seg_snr_db(ref_n, deg_n, rate_r)
    lsd = lsd_db(ref_n, deg_n, rate_r)
    pesq = pesq_wb(ref_n, deg_n, rate_r)
    return {
        "snr_db": snr,
        "seg_snr_db": seg,
        "lsd_db": lsd,
        "pesq_wb": pesq,
        "lag_samples": int(lag),
        "lag_ms": 1000.0 * lag / rate_r,
        "score": seg,
        "score_kind": "seg_snr_db",
    }


def self_check():
    if np is None:
        raise SystemExit("numpy is required")
    rate = 24000
    t = np.arange(rate) / rate
    tone = 8000 * np.sin(2 * math.pi * 440 * t)
    identical = score_pair_arrays(tone, tone, rate)
    if identical["snr_db"] < 80 or identical["lsd_db"] > 0.1:
        raise SystemExit(f"identical signals scored too low: {identical}")
    late = np.concatenate([np.zeros(100), tone[:-100]])
    delayed = score_pair_arrays(tone, late, rate)
    if abs(delayed["lag_samples"] - 100) > 1 or delayed["snr_db"] < 40:
        raise SystemExit(f"100-sample delay was not recovered: {delayed}")
    rng = np.random.default_rng(2)
    junk = rng.standard_normal(rate) * 8000
    bad = score_pair_arrays(tone, junk, rate)
    if bad["seg_snr_db"] > 5:
        raise SystemExit(f"noise should not look like the tone: {bad}")
    print(
        f"metrics self-check ok identical_snr={identical['snr_db']:.1f} "
        f"delay_lag={delayed['lag_samples']} noise_seg={bad['seg_snr_db']:.1f}"
    )


def score_pair_arrays(ref, deg, rate):
    dest = Path("/tmp")
    # Use a private temp so parallel runs do not collide.
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        r = Path(tmp) / "ref.wav"
        d = Path(tmp) / "deg.wav"
        write_wav(r, rate, ref)
        write_wav(d, rate, deg)
        return score_pair(r, d)


COLUMNS = [
    "stage",
    "clip",
    "codec",
    "compute",
    "encoder",
    "pack",
    "fpp",
    "aec",
    "loss_pct",
    "jitter_ms",
    "opus_bps",
    "path",
    "status",
    "score",
    "score_kind",
    "snr_db",
    "seg_snr_db",
    "lsd_db",
    "pesq_wb",
    "out_of_codebook",
    "clamped",
    "onwire_kbps",
    "latency_ms",
    "git_sha",
    "detail",
]


def append_row(csv_path, fields):
    path = Path(csv_path)
    path.parent.mkdir(parents=True, exist_ok=True)
    new = not path.exists()
    row = {key: fields.get(key, "") for key in COLUMNS}
    with path.open("a", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=COLUMNS)
        if new:
            writer.writeheader()
        writer.writerow(row)


def write_summary(csv_path, md_path):
    path = Path(csv_path)
    rows = []
    if path.exists():
        with path.open() as f:
            rows = list(csv.DictReader(f))
    lines = [
        "# LiquidCodec quality ladder",
        "",
        "Score is segmental SNR in dB after cross-correlation alignment, except packing rows, whose score is 1 when the codes match bit for bit. A regression is a lower score on the same stage, clip, and settings.",
        "",
        "| stage | clip | codec | settings | status | score | snr | lsd | pesq | out of codebook | on-wire kbps | latency ms |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for row in rows:
        settings = " ".join(
            part
            for part in (
                row.get("compute") or "",
                row.get("encoder") or "",
                f"pack={row['pack']}" if row.get("pack") else "",
                f"fpp={row['fpp']}" if row.get("fpp") else "",
                f"aec={row['aec']}" if row.get("aec") else "",
                f"loss={row['loss_pct']}" if row.get("loss_pct") not in ("", None) else "",
                f"jitter={row['jitter_ms']}" if row.get("jitter_ms") not in ("", None) else "",
                f"opus={row['opus_bps']}" if row.get("opus_bps") else "",
                row.get("path") or "",
            )
            if part
        )
        lines.append(
            "| {stage} | {clip} | {codec} | {settings} | {status} | {score} | {snr} | {lsd} | {pesq} | {bad} | {kbps} | {lat} |".format(
                stage=row.get("stage", ""),
                clip=row.get("clip", ""),
                codec=row.get("codec", ""),
                settings=settings,
                status=row.get("status", ""),
                score=row.get("score", ""),
                snr=row.get("snr_db", ""),
                lsd=row.get("lsd_db", ""),
                pesq=row.get("pesq_wb", ""),
                bad=row.get("out_of_codebook", ""),
                kbps=row.get("onwire_kbps", ""),
                lat=row.get("latency_ms", ""),
            )
        )
    lines.append("")
    Path(md_path).write_text("\n".join(lines))
    print(f"wrote {md_path}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-check", action="store_true")
    parser.add_argument("--clips", metavar="DIR")
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--score", nargs=2, metavar=("REF", "DEG"))
    parser.add_argument("--append-csv")
    parser.add_argument("--field", action="append", default=[])
    parser.add_argument("--summary", nargs=2, metavar=("CSV", "MD"))
    args = parser.parse_args()
    if args.self_check:
        self_check()
    if args.clips:
        for path in generate_clips(args.clips, args.quick):
            print(path)
    if args.score:
        result = score_pair(args.score[0], args.score[1])
        pesq = "na" if result["pesq_wb"] is None else f"{result['pesq_wb']:.3f}"
        print(
            "SCORE snr_db={snr_db:.3f} seg_snr_db={seg_snr_db:.3f} lsd_db={lsd_db:.3f} "
            "pesq_wb={pesq} lag_samples={lag_samples} score={score:.3f}".format(pesq=pesq, **result)
        )
    if args.append_csv:
        fields = {}
        for item in args.field:
            if "=" not in item:
                continue
            key, value = item.split("=", 1)
            fields[key] = value
        append_row(args.append_csv, fields)
    if args.summary:
        write_summary(args.summary[0], args.summary[1])


if __name__ == "__main__":
    main()
