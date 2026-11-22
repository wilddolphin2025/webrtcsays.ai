#!/usr/bin/env python3
"""LiquidCodec vs Opus on-wire bandwidth, plus a quality chart when a Mac run exists.

--theoretical prints the expected on-wire rates and does not place a call.
--analyze reads a bench directory produced by bench_bandwidth.sh.
"""

import argparse
import csv
import json
import math
import os
import sys


IP = 20
UDP = 8
RTP = 12
SRTP = 16
LIQUID_FPS = 12.5


def payload_bytes(codec, pack, frames, opus_bps=0, ptime_ms=20):
    if codec == "liquid":
        if frames not in (1, 2, 4):
            raise ValueError(frames)
        if pack == "tight":
            return (frames * 12 * 11 + 7) // 8
        if pack == "u16":
            return 4 + frames * 24
        raise ValueError(pack)
    # Opus payload size is not fixed. This is the nominal target, before DTX.
    return max(1, int(round(opus_bps * ptime_ms / 8000.0)))


def packets_per_second(codec, frames, ptime_ms):
    if codec == "liquid":
        return LIQUID_FPS / frames
    return 1000.0 / ptime_ms


def on_wire_bps(payload, pps, srtp_tag):
    return (payload + IP + UDP + RTP + srtp_tag) * 8.0 * pps


def liquid_rows():
    rows = []
    for pack in ("u16", "tight"):
        for frames in (1, 2, 4):
            payload = payload_bytes("liquid", pack, frames)
            pps = packets_per_second("liquid", frames, 0)
            rows.append({
                "config": "liquid_%s_fpp%d" % (pack, frames),
                "codec": "liquid",
                "pack": pack,
                "frames_per_packet": frames,
                "ptime_ms": 80 * frames,
                "opus_kbps": "",
                "dtx": "",
                "payload_bytes": payload,
                "packets_per_s": round(pps, 4),
                "payload_kbps": round(payload * 8.0 * pps / 1000.0, 4),
                "onwire_ipv4_kbps": round(on_wire_bps(payload, pps, 0) / 1000.0, 4),
                "onwire_srtp16_kbps": round(on_wire_bps(payload, pps, SRTP) / 1000.0, 4),
                "notes": "exact codec payload. tight has no payload header; RTP seq/timestamp identify the packet",
            })
    return rows


def opus_rows():
    rows = []
    for kbps in (6, 8, 12, 16, 24, 32):
        for ptime in (20, 40, 60):
            for dtx in (0, 1):
                payload = payload_bytes("opus", "", 1, kbps * 1000, ptime)
                pps = packets_per_second("opus", 1, ptime)
                rows.append({
                    "config": "opus_%dk_p%d_dtx%d" % (kbps, ptime, dtx),
                    "codec": "opus",
                    "pack": "",
                    "frames_per_packet": "",
                    "ptime_ms": ptime,
                    "opus_kbps": kbps,
                    "dtx": dtx,
                    "payload_bytes": payload,
                    "packets_per_s": round(pps, 4),
                    "payload_kbps": round(payload * 8.0 * pps / 1000.0, 4),
                    "onwire_ipv4_kbps": round(on_wire_bps(payload, pps, 0) / 1000.0, 4),
                    "onwire_srtp16_kbps": round(on_wire_bps(payload, pps, SRTP) / 1000.0, 4),
                    "notes": "nominal Opus target. DTX sends less during silence; that only shows up in a measured run",
                })
    return rows


def write_csv(path, rows):
    fields = ["config", "codec", "pack", "frames_per_packet", "ptime_ms", "opus_kbps", "dtx",
              "payload_bytes", "packets_per_s", "payload_kbps", "onwire_ipv4_kbps",
              "onwire_srtp16_kbps", "notes"]
    extra = []
    for row in rows:
        for key in row:
            if key not in fields and key not in extra:
                extra.append(key)
    fields.extend(extra)
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def write_svg(path, rows, x_key, y_key, title):
    points = []
    for row in rows:
        try:
            x = float(row.get(x_key) or "")
            y = float(row.get(y_key) or "")
        except ValueError:
            continue
        if math.isnan(x) or math.isnan(y):
            continue
        points.append((x, y, row.get("config", "")))
    if not points:
        return False
    width, height, pad = 760, 420, 48
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    xmin, xmax = min(xs), max(xs)
    ymin, ymax = min(ys), max(ys)
    if xmax == xmin:
        xmax = xmin + 1
    if ymax == ymin:
        ymax = ymin + 1

    def sx(x):
        return pad + (x - xmin) / (xmax - xmin) * (width - 2 * pad)

    def sy(y):
        return height - pad - (y - ymin) / (ymax - ymin) * (height - 2 * pad)

    colors = ["#1f4b99", "#c4521a", "#2a7a3b", "#6b3fa0", "#a33b3b", "#1d6a75"]
    groups = {}
    for x, y, name in points:
        family = name.split("_")[0] if name else "run"
        groups.setdefault(family, []).append((x, y, name))
    lines = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d">' % (width, height),
        '<rect width="100%%" height="100%%" fill="#fbfbf8"/>',
        '<text x="%d" y="24" font-family="sans-serif" font-size="16">%s</text>' % (pad, title),
        '<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="#222"/>' % (pad, height - pad, width - pad, height - pad),
        '<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="#222"/>' % (pad, pad, pad, height - pad),
        '<text x="%d" y="%d" font-family="sans-serif" font-size="12">%s</text>' % (
            width / 2, height - 8, x_key),
        '<text x="16" y="%d" font-family="sans-serif" font-size="12" transform="rotate(-90 16 %d)">%s</text>' % (
            height / 2, height / 2, y_key),
    ]
    for i, (family, pts) in enumerate(sorted(groups.items())):
        color = colors[i % len(colors)]
        pts.sort()
        poly = " ".join("%.1f,%.1f" % (sx(x), sy(y)) for x, y, _ in pts)
        lines.append('<polyline fill="none" stroke="%s" stroke-width="2" points="%s"/>' % (color, poly))
        for x, y, name in pts:
            lines.append('<circle cx="%.1f" cy="%.1f" r="3.5" fill="%s"><title>%s</title></circle>' % (
                sx(x), sy(y), color, name))
        lines.append('<text x="%d" y="%d" fill="%s" font-family="sans-serif" font-size="12">%s</text>' % (
            width - pad - 80, 40 + 16 * i, color, family))
    lines.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(lines))
    return True


def word_error_rate(reference, hypothesis):
    ref = reference.split()
    hyp = hypothesis.split()
    if not ref:
        return None
    rows = len(ref) + 1
    cols = len(hyp) + 1
    dist = list(range(cols))
    for i in range(1, rows):
        prev = dist[0]
        dist[0] = i
        for j in range(1, cols):
            cur = dist[j]
            cost = 0 if ref[i - 1] == hyp[j - 1] else 1
            dist[j] = min(dist[j] + 1, dist[j - 1] + 1, prev + cost)
            prev = cur
    return dist[-1] / float(len(ref))


def try_quality(reference_wav, decoded_wav):
    scores = {"visqol": None, "pesq": None, "stoi": None, "quality_tool": ""}
    try:
        import numpy as np
        import soundfile as sf
        ref, sr_ref = sf.read(reference_wav)
        dec, sr_dec = sf.read(decoded_wav)
    except Exception as exc:
        scores["quality_tool"] = "audio read failed: %s" % exc
        return scores
    try:
        from pesq import pesq
        scores["pesq"] = float(pesq(16000, _resample(ref, sr_ref, 16000), _resample(dec, sr_dec, 16000), "wb"))
        scores["quality_tool"] = "pesq"
    except Exception:
        pass
    try:
        from pystoi import stoi
        scores["stoi"] = float(stoi(_resample(ref, sr_ref, 16000), _resample(dec, sr_dec, 16000), 16000, extended=False))
    except Exception:
        pass
    visqol = _visqol(reference_wav, decoded_wav)
    if visqol is not None:
        scores["visqol"] = visqol
        scores["quality_tool"] = "visqol"
    if not scores["quality_tool"]:
        scores["quality_tool"] = "not_installed"
    return scores


def _resample(samples, src, dst):
    import numpy as np
    samples = np.asarray(samples, dtype=np.float64)
    if samples.ndim > 1:
        samples = samples.mean(axis=1)
    if src == dst:
        return samples
    count = int(round(len(samples) * float(dst) / float(src)))
    if count <= 1:
        return samples
    x_old = np.linspace(0.0, 1.0, len(samples))
    x_new = np.linspace(0.0, 1.0, count)
    return np.interp(x_new, x_old, samples)


def _visqol(reference, decoded):
    try:
        from visqol import visqol_lib_py
        from visqol.pb2 import visqol_config_pb2
        cfg = visqol_config_pb2.VisqolConfig()
        cfg.audio.sample_rate = 16000
        cfg.options.use_speech_scoring = True
        api = visqol_lib_py.VisqolApi()
        api.Create(cfg)
        return float(api.Measure(reference, decoded).moslqo)
    except Exception:
        return None


def transcribe(path):
    try:
        import whisper
        model = whisper.load_model(os.environ.get("WHISPER_MODEL", "tiny"))
        return model.transcribe(path).get("text", "").strip()
    except Exception:
        return ""


def load_stats(path):
    last = {}
    if not os.path.isfile(path):
        return last
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            audio = obj.get("audio") if isinstance(obj, dict) else None
            if isinstance(audio, dict):
                last = audio
                last["rtt_ms"] = obj.get("rtt_ms")
                last["jitter_buffer_ms"] = obj.get("jitter_buffer_ms")
    return last


def measured_kbps(stats, seconds):
    if seconds <= 0:
        return None, None
    packets = float(stats.get("send_packets") or 0)
    header = float(stats.get("send_header_bytes") or 0)
    total = float(stats.get("send_bytes") or 0)
    if packets <= 0 or total <= 0:
        return None, None
    payload = max(0.0, total - header)
    plain = (payload + header + packets * (IP + UDP)) * 8.0 / seconds / 1000.0
    srtp = (payload + header + packets * (IP + UDP + SRTP)) * 8.0 / seconds / 1000.0
    return plain, srtp


def analyze(run_dir, out_csv):
    rows = []
    for name in sorted(os.listdir(run_dir)):
        folder = os.path.join(run_dir, name)
        meta_path = os.path.join(folder, "meta.json")
        if not os.path.isfile(meta_path):
            continue
        with open(meta_path) as f:
            meta = json.load(f)
        stats = load_stats(os.path.join(folder, "caller.log"))
        seconds = float(meta.get("duration_s") or 0)
        plain, srtp = measured_kbps(stats, seconds)
        row = dict(meta)
        row["config"] = name
        row["send_bytes"] = stats.get("send_bytes", "")
        row["send_packets"] = stats.get("send_packets", "")
        row["send_header_bytes"] = stats.get("send_header_bytes", "")
        row["measured_onwire_ipv4_kbps"] = "" if plain is None else round(plain, 4)
        row["measured_onwire_srtp16_kbps"] = "" if srtp is None else round(srtp, 4)
        row["rtt_ms"] = stats.get("rtt_ms", "")
        row["jitter_buffer_ms"] = stats.get("jitter_buffer_ms", "")
        decoded = meta.get("decoded_wav") or ""
        reference = meta.get("reference_wav") or ""
        if reference and decoded and os.path.isfile(reference) and os.path.isfile(decoded):
            scores = try_quality(reference, decoded)
            row.update(scores)
            transcript = meta.get("transcript") or ""
            if not transcript and os.environ.get("LIQUID_BENCH_WHISPER") == "1":
                transcript = transcribe(reference)
                hypo = transcribe(decoded)
                if transcript and hypo:
                    row["wer"] = word_error_rate(transcript.lower(), hypo.lower())
            elif transcript and os.environ.get("LIQUID_BENCH_WHISPER") == "1":
                hypo = transcribe(decoded)
                if hypo:
                    row["wer"] = word_error_rate(transcript.lower(), hypo.lower())
        algo = meta.get("algorithmic_latency_ms")
        jb = stats.get("jitter_buffer_ms")
        rtt = stats.get("rtt_ms")
        decode_ms = meta.get("prediction_ms_mean")
        if algo is not None and isinstance(jb, (int, float)) and isinstance(rtt, (int, float)):
            extra = float(decode_ms or 0)
            row["mouth_to_ear_estimate_ms"] = round(float(algo) + float(jb) + float(rtt) / 2.0 + extra, 2)
        rows.append(row)
    write_csv(out_csv, rows)
    chart = os.path.splitext(out_csv)[0] + ".svg"
    y_key = "visqol" if any(row.get("visqol") not in (None, "") for row in rows) else "pesq"
    x_key = "measured_onwire_ipv4_kbps" if any(row.get(y_key) not in (None, "") and row.get("measured_onwire_ipv4_kbps") not in ("", None) for row in rows) else "onwire_ipv4_kbps"
    wrote = write_svg(chart, rows, x_key, y_key, "Quality vs on-wire kbps")
    wer_chart = os.path.splitext(out_csv)[0] + "_wer.svg"
    if any(row.get("wer") not in (None, "") for row in rows):
        write_svg(wer_chart, rows, x_key, "wer", "WER vs on-wire kbps")
    return rows, chart if wrote else ""


def self_check():
    assert payload_bytes("liquid", "tight", 1) == 17
    assert payload_bytes("liquid", "tight", 2) == 33
    assert payload_bytes("liquid", "tight", 4) == 66
    assert payload_bytes("liquid", "u16", 1) == 28
    assert payload_bytes("liquid", "u16", 2) == 52
    assert payload_bytes("liquid", "u16", 4) == 100
    assert abs(payload_bytes("liquid", "tight", 1) * 8 * 12.5 - 1700) < 0.1
    assert abs(on_wire_bps(17, 12.5, 0) - 5700) < 0.1
    assert abs(on_wire_bps(17, 12.5, 16) - 7300) < 0.1
    assert abs(word_error_rate("the cat sat", "the cat sat") - 0) < 1e-9
    assert abs(word_error_rate("the cat sat", "the dog sat") - (1.0 / 3.0)) < 1e-9
    folder = os.path.join(os.path.dirname(__file__), "out")
    os.makedirs(folder, exist_ok=True)
    rows = liquid_rows() + opus_rows()
    path = os.path.join(folder, "theoretical.csv")
    write_csv(path, rows)
    assert write_svg(os.path.join(folder, "theoretical.svg"), rows, "onwire_ipv4_kbps", "payload_kbps",
                     "Theoretical payload vs on-wire")
    print("bandwidth.py self-check passed (%d theoretical rows)" % len(rows))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--theoretical", action="store_true")
    parser.add_argument("--analyze")
    parser.add_argument("--self-check", action="store_true")
    parser.add_argument("--out", default="")
    args = parser.parse_args()
    if args.self_check:
        self_check()
        return 0
    if args.theoretical:
        rows = liquid_rows() + opus_rows()
        out = args.out or "theoretical_bandwidth.csv"
        write_csv(out, rows)
        svg = os.path.splitext(out)[0] + ".svg"
        write_svg(svg, rows, "onwire_ipv4_kbps", "payload_kbps", "Theoretical payload vs IPv4/UDP/RTP kbps")
        print("wrote %s and %s" % (out, svg))
        return 0
    if args.analyze:
        out = args.out or os.path.join(args.analyze, "bandwidth.csv")
        rows, chart = analyze(args.analyze, out)
        print("wrote %s (%d runs)%s" % (out, len(rows), (" and " + chart) if chart else ""))
        return 0
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
