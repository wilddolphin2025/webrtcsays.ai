#!/usr/bin/env bash
# LiquidCall vs Opus bandwidth matrix for an Apple Silicon Mac.
# This script does not run sudo. --pcap and --impair print the commands.
# --theoretical needs no models and no directcall.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=layout.sh
source "${SCRIPT_DIR}/layout.sh"
# Binary and models are whatever ./build.sh release liquidcall wrote.
BIN="${BIN:-${LC_BIN}}"
MODEL_DIR="${LIQUIDCALL_DEST:-${LIQUIDCODEC_DEST:-${LC_MODELS}}}"
OUT="${OUT:-${LC_TREE}/scripts/liquidcall/out/bandwidth}"
SAMPLES="${LIQUIDCALL_SAMPLES:-${LIQUIDCODEC_SAMPLES:-}}"
CONVERSATION="${LIQUIDCALL_CONVERSATION:-${LIQUIDCODEC_CONVERSATION:-}}"
PORT="${LIQUID_BENCH_PORT:-3460}"
DURATION="${LIQUID_BENCH_DURATION:-12}"
QUICK=0
THEORETICAL=0
PCAP=0
IMPAIR=0
RUN_SUDO=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --theoretical) THEORETICAL=1; shift ;;
    --quick) QUICK=1; shift ;;
    --pcap) PCAP=1; shift ;;
    --impair) IMPAIR=1; shift ;;
    --run-sudo) RUN_SUDO=1; shift ;;
    --bin) BIN="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --samples) SAMPLES="$2"; shift 2 ;;
    --conversation) CONVERSATION="$2"; shift 2 ;;
    --duration) DURATION="$2"; shift 2 ;;
    -h|--help)
      echo "usage: $0 [--theoretical] [--quick] [--pcap] [--impair] [--run-sudo]"
      echo "       [--samples DIR] [--conversation WAV] [--bin PATH] [--out DIR]"
      exit 0
      ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

mkdir -p "${OUT}"
python3 "${SCRIPT_DIR}/bandwidth.py" --theoretical --out "${OUT}/theoretical.csv"

if [[ "${THEORETICAL}" == "1" ]]; then
  echo "Theoretical table only: ${OUT}/theoretical.csv"
  exit 0
fi

if [[ ! -x "${BIN}" ]]; then
  echo "directcall is not at ${BIN}." >&2
  echo "From the gclient parent: ./build.sh release liquidcall" >&2
  echo "The theoretical table is still at ${OUT}/theoretical.csv" >&2
  exit 1
fi
if [[ ! -f "${MODEL_DIR}/liquidcall.json" ]]; then
  echo "No compiled models at ${MODEL_DIR}." >&2
  echo "From the gclient parent: ./build.sh release liquidcall" >&2
  exit 1
fi

clips=()
if [[ -n "${SAMPLES}" && -d "${SAMPLES}" ]]; then
  while IFS= read -r f; do clips+=("$f"); done < <(find "${SAMPLES}" -type f \( -name '*.wav' -o -name '*.flac' \) | sort)
fi
if [[ -n "${CONVERSATION}" && -f "${CONVERSATION}" ]]; then
  clips+=("${CONVERSATION}")
fi
if [[ ${#clips[@]} -eq 0 ]]; then
  python3 "${SCRIPT_DIR}/tone_wav.py" "${OUT}/tone.wav" 3
  clips=("${OUT}/tone.wav")
  echo "No --samples clip. Using ${OUT}/tone.wav from this script. Pass --samples for speech and WER."
fi
if [[ "${QUICK}" == "1" ]]; then
  clips=("${clips[0]}")
fi

pcap_note() {
  local name="$1"
  echo "pcap cross-check (not started):"
  echo "  sudo tcpdump -i lo0 -n -w ${OUT}/${name}/lo0.pcap 'udp and (port ${PORT} or portrange 10000-60000)'"
  echo "Re-run with --pcap --run-sudo only if you want this script to execute that command."
}

impair_note() {
  local loss="$1" bw="$2"
  echo "Impaired network is not applied. To opt in, run these yourself, then the bench, then delete the pipe:"
  echo "  sudo dnctl pipe 1 config bw ${bw}Kbit/s delay 40 plr 0.0${loss}"
  echo "  echo 'dummynet out proto udp from any to any pipe 1' | sudo pfctl -f -"
  echo "  sudo pfctl -e || true"
  echo "  # restore: sudo pfctl -d; sudo dnctl -q flush"
}

run_pair() {
  local name="$1"
  local inject="$2"
  shift 2
  local dir="${OUT}/${name}"
  mkdir -p "${dir}/callee" "${dir}/caller"
  local callee_log="${dir}/callee.log"
  local caller_log="${dir}/caller.log"
  if [[ "${PCAP}" == "1" ]]; then
    pcap_note "${name}" | tee "${dir}/pcap.txt"
    if [[ "${RUN_SUDO}" == "1" ]]; then
      sudo tcpdump -i lo0 -n -w "${dir}/lo0.pcap" "udp and port ${PORT}" &
      echo $! > "${dir}/tcpdump.pid"
    fi
  fi
  "${BIN}" --mode=callee --user_name=alice --room_name=liquid \
    --no-encryption --no-websocket_signaling --no-video --no-whisper --no-llama \
    --stats=2 --record-dir="${dir}/callee" \
    "$@" \
    "127.0.0.1:${PORT}" >"${callee_log}" 2>&1 &
  local callee_pid=$!
  sleep 1
  "${BIN}" --mode=caller --user_name=bob --target_name=alice --room_name=liquid \
    --no-encryption --no-websocket_signaling --no-video --no-whisper --no-llama \
    --stats=2 --duration="${DURATION}" --record-dir="${dir}/caller" \
    --inject-wav="${inject}" \
    "$@" \
    "127.0.0.1:${PORT}" >"${caller_log}" 2>&1 || true
  sleep 1
  kill "${callee_pid}" 2>/dev/null || true
  wait "${callee_pid}" 2>/dev/null || true
  if [[ -f "${dir}/tcpdump.pid" ]]; then
    kill "$(cat "${dir}/tcpdump.pid")" 2>/dev/null || true
  fi
  cp "${caller_log}" "${dir}/caller.log"
}

for clip in "${clips[@]}"; do
  base="$(basename "${clip}")"
  base="${base%.*}"
  if [[ "${QUICK}" == "1" ]]; then
    liquid_packs=(tight)
    liquid_fpps=(1)
    liquid_decodes=(ane)
    opus_rates=(6 32)
    opus_ptimes=(20)
    opus_dtxs=(0)
  else
    liquid_packs=(u16 tight)
    liquid_fpps=(1 2 4)
    liquid_decodes=(ane cpu)
    opus_rates=(6 8 12 16 24 32)
    opus_ptimes=(20 40 60)
    opus_dtxs=(0 1)
  fi
  for pack in "${liquid_packs[@]}"; do
    for fpp in "${liquid_fpps[@]}"; do
      for decode in "${liquid_decodes[@]}"; do
        name="liquid_${pack}_fpp${fpp}_${decode}_${base}"
        run_pair "${name}" "${clip}" \
          --audio-codec=liquidcall --liquid-decode="${decode}" \
          --liquid-model-dir="${MODEL_DIR}" \
          --liquid-pack="${pack}" --liquid-frames-per-packet="${fpp}"
        # Streaming encoder: first packet waits 80+20 ms, then (fpp-1) more
        # frames, plus 20 ms of decoder overlap-add.
        algo_ms=$((fpp * 80 + 40))
        cat > "${OUT}/${name}/meta.json" <<EOF
{"codec":"liquid","pack":"${pack}","frames_per_packet":${fpp},"decode":"${decode}","clip":"${clip}","duration_s":${DURATION},"reference_wav":"${clip}","decoded_wav":"${OUT}/${name}/callee/codec_out_24k.wav","algorithmic_latency_ms":${algo_ms}}
EOF
      done
    done
  done
  for rate in "${opus_rates[@]}"; do
    for ptime in "${opus_ptimes[@]}"; do
      for dtx in "${opus_dtxs[@]}"; do
        name="opus_${rate}k_p${ptime}_dtx${dtx}_${base}"
        dtx_flag=(--no-opus-dtx)
        if [[ "${dtx}" == "1" ]]; then dtx_flag=(--opus-dtx); fi
        run_pair "${name}" "${clip}" \
          --audio-codec=opus --opus-bitrate="$((rate * 1000))" --opus-ptime="${ptime}" \
          "${dtx_flag[@]}"
        cat > "${OUT}/${name}/meta.json" <<EOF
{"codec":"opus","opus_kbps":${rate},"ptime_ms":${ptime},"dtx":${dtx},"clip":"${clip}","duration_s":${DURATION},"reference_wav":"${clip}","decoded_wav":"${OUT}/${name}/callee/output_48k.wav"}
EOF
      done
    done
  done
done

if [[ "${IMPAIR}" == "1" ]]; then
  {
    echo "Optional impaired-network pass. These commands are printed, not run, unless you passed --run-sudo."
    for loss in 1 3 5 10; do
      impair_note "${loss}" 64
    done
    echo "Caps: sudo dnctl pipe 1 config bw 8Kbit/s delay 20"
    echo "      sudo dnctl pipe 1 config bw 16Kbit/s delay 20"
  } | tee "${OUT}/impair.txt"
  if [[ "${RUN_SUDO}" == "1" ]]; then
    echo "--run-sudo was set, but this script still does not apply dnctl/pfctl." >&2
    echo "Run the commands in ${OUT}/impair.txt yourself, then re-run a single config." >&2
  fi
fi

python3 "${SCRIPT_DIR}/bandwidth.py" --analyze "${OUT}" --out "${OUT}/bandwidth.csv"
echo "Measured table: ${OUT}/bandwidth.csv"
echo "Quality tools, if you want ViSQOL / PESQ / STOI / Whisper on the WAVs:"
echo "  python3 -m pip install pesq pystoi soundfile numpy"
echo "  # ViSQOL speech mode: pip install visqol   (or the C++ visqol binary)"
echo "  # Whisper: pip install openai-whisper, then LIQUID_BENCH_WHISPER=1"
echo "  # A transcript file next to a clip is used as the WER reference when Whisper is enabled."
