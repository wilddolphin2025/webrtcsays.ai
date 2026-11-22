#!/usr/bin/env bash
# Incremental LiquidCodec quality ladder. Each stage can run alone.
# The script generates the clips, creates a venv for numpy (and PESQ when
# the wheel builds), compiles the packing test, and on macOS builds
# directcall and fetches models when they are missing.
#
#   src/scripts/liquidcodec/quality_ladder.sh
#   src/scripts/liquidcodec/quality_ladder.sh --stages 2
#   src/scripts/liquidcodec/quality_ladder.sh --stages 1 --quick
#   src/scripts/liquidcodec/quality_ladder.sh --stages 1-6
#
# Stage 1  offline codec round-trip (Core ML; skipped without the Apple build)
# Stage 2  u16 and tight pack round-trip, frames-per-packet 1, 2, 4
# Stage 3  AudioEncoder / AudioDecoder / NetEq, no network
# Stage 4  two directcall processes on 127.0.0.1, AEC off then on
# Stage 5  loss 0/1/3/5/10% and jitter 0/20/50 ms, in-process, no sudo
# Stage 6  Opus at 6, 8, 12, 16 kbps through stages 3-5
#
# --quick uses the vowel clip only, CPU int8, loss 0 and 5, jitter 0 and 20,
# and Opus at 8 kbps. Score is segmental SNR after alignment, or 1 for a
# bit-exact pack. Results land in scripts/liquidcodec/out/quality/<timestamp>/.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=layout.sh
source "${SCRIPT_DIR}/layout.sh"

STAGES="1,2,3,4,5,6"
QUICK=0
OUT=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --stages) STAGES="$2"; shift 2 ;;
    --quick) QUICK=1; shift ;;
    --out) OUT="$2"; shift 2 ;;
    -h|--help)
      sed -n '2,22p' "$0"
      exit 0
      ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [[ -z "${OUT}" ]]; then
  OUT="${SCRIPT_DIR}/out/quality/$(date -u +%Y%m%dT%H%M%SZ)"
fi
mkdir -p "${OUT}"
CSV="${OUT}/quality.csv"
MD="${OUT}/quality.md"
PY="${SCRIPT_DIR}/quality_metrics.py"
SHA="$(git -C "${LC_TREE}" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
echo "quality ladder ${SHA} -> ${OUT}"

expand_stages() {
  local spec="$1" part start end i
  local -a found=()
  IFS=',' read -r -a parts <<<"${spec}"
  for part in "${parts[@]}"; do
    if [[ "${part}" =~ ^([0-9]+)-([0-9]+)$ ]]; then
      start="${BASH_REMATCH[1]}"
      end="${BASH_REMATCH[2]}"
      for ((i = start; i <= end; i++)); do found+=("${i}"); done
    else
      found+=("${part}")
    fi
  done
  printf '%s\n' "${found[@]}"
}

want_stage() {
  local n="$1" s
  while IFS= read -r s; do
    [[ "${s}" == "${n}" ]] && return 0
  done < <(expand_stages "${STAGES}")
  return 1
}

# numpy is required. PESQ is optional; a failed wheel does not stop the ladder.
if [[ ! -x "${SCRIPT_DIR}/.venv/bin/python" ]]; then
  rm -rf "${SCRIPT_DIR}/.venv"
  if ! python3 -m venv "${SCRIPT_DIR}/.venv"; then
    echo "python3-venv is not installed; scoring with the system python3."
    echo "On Debian: apt install python3-venv"
  fi
fi
if [[ -x "${SCRIPT_DIR}/.venv/bin/python" ]]; then
  PYBIN="${SCRIPT_DIR}/.venv/bin/python"
else
  PYBIN="$(command -v python3)"
fi
"${PYBIN}" -m pip install --quiet numpy || {
  echo "numpy install failed" >&2
  exit 1
}
"${PYBIN}" -m pip install --quiet pesq || echo "PESQ not installed; pesq_wb will be empty. Debian needs python3-dev to build it."

"${PYBIN}" "${PY}" --self-check

if [[ "${QUICK}" == "1" ]]; then
  SECONDS_CLIP=0.8
  CALL_SECONDS=6
else
  SECONDS_CLIP=2.0
  CALL_SECONDS=8
fi
if [[ "${QUICK}" == "1" ]]; then
  "${PYBIN}" "${PY}" --clips "${OUT}/clips" --quick >"${OUT}/clips.txt"
else
  "${PYBIN}" "${PY}" --clips "${OUT}/clips" >"${OUT}/clips.txt"
fi
CLIPS=()
while IFS= read -r clip_path; do
  [[ -n "${clip_path}" ]] && CLIPS+=("${clip_path}")
done <"${OUT}/clips.txt"
if [[ "${QUICK}" == "1" && ${#CLIPS[@]} -gt 0 ]]; then
  CLIPS=("${CLIPS[0]}")
fi

if [[ -n "${LC_PARENT:-}" && -f "${LC_PARENT}/build.sh" ]]; then
  LC_ROOT="${LC_PARENT}"
elif [[ -f "${LC_TREE}/build.sh" ]]; then
  LC_ROOT="${LC_TREE}"
else
  LC_ROOT="$(cd "${LC_TREE}/.." && pwd)"
fi

ensure_binary() {
  local need=0
  if [[ ! -x "${LC_BIN}" ]]; then
    need=1
  else
    local help
    help="$("${LC_BIN}" --help 2>&1 || true)"
    if ! grep -q -- '--quality=' <<<"${help}"; then
      need=1
    fi
  fi
  if [[ "${need}" == "0" ]]; then
    return 0
  fi
  if [[ "$(uname -s)" != "Darwin" || ! -f "${LC_ROOT}/build.sh" ]]; then
    return 1
  fi
  echo "building directcall (release liquidcodec)"
  (cd "${LC_ROOT}" && ./build.sh -b "${SHA}" release liquidcodec)
  [[ -x "${LC_BIN}" ]]
}

ensure_models() {
  local fetch="${LC_TREE}/scripts/liquidcodec/fetch_models.sh"
  [[ -f "${fetch}" ]] || fetch="${LC_ROOT}/src/scripts/liquidcodec/fetch_models.sh"
  [[ -f "${fetch}" ]] || fetch="${LC_ROOT}/scripts/liquidcodec/fetch_models.sh"
  if [[ ! -f "${fetch}" ]]; then
    return 1
  fi
  "${fetch}" --dest "${LC_MODELS}"
}

add_row() {
  local args=() field
  for field in "$@"; do args+=(--field "${field}"); done
  "${PYBIN}" "${PY}" --append-csv "${CSV}" "${args[@]}" --field "git_sha=${SHA}"
}

score_wav() {
  local ref="$1" deg="$2"
  if [[ ! -f "${deg}" ]]; then
    echo "SCORE snr_db= seg_snr_db= lsd_db= pesq_wb=na"
    return 1
  fi
  "${PYBIN}" "${PY}" --score "${ref}" "${deg}"
}

parse_score() {
  # Sets SCORE_SNR SCORE_SEG SCORE_LSD SCORE_PESQ from a SCORE line.
  local line="$1"
  SCORE_SNR="$(sed -n 's/.*snr_db=\([^ ]*\).*/\1/p' <<<"${line}")"
  SCORE_SEG="$(sed -n 's/.*seg_snr_db=\([^ ]*\).*/\1/p' <<<"${line}")"
  SCORE_LSD="$(sed -n 's/.*lsd_db=\([^ ]*\).*/\1/p' <<<"${line}")"
  SCORE_PESQ="$(sed -n 's/.*pesq_wb=\([^ ]*\).*/\1/p' <<<"${line}")"
  [[ "${SCORE_PESQ}" == "na" ]] && SCORE_PESQ=""
}

run_quality_bin() {
  local stage="$1" codec="$2" compute="$3" encoder="$4" loss="$5" jitter="$6" opus="$7" clip="$8" dir="$9"
  mkdir -p "${dir}"
  local args=(
    --quality="${stage}"
    --audio-codec="${codec}"
    --inject-wav="${clip}"
    --record-dir="${dir}"
    --no-video --no-whisper --no-llama --no-encryption --no-websocket_signaling
    --impair-loss="${loss}"
    --impair-jitter-ms="${jitter}"
  )
  if [[ "${codec}" == "liquidcodec" ]]; then
    args+=(--liquid-decode="${compute}" --liquid-encoder="${encoder}" --liquid-model-dir="${LC_MODELS}" --aec=off)
  fi
  if [[ -n "${opus}" && "${opus}" != "0" ]]; then
    args+=(--opus-bitrate="${opus}")
  fi
  set +e
  "${LC_BIN}" "${args[@]}" >"${dir}/directcall.log" 2>&1
  local rc=$?
  set -e
  echo "${rc}"
}

stage2() {
  local bin="${OUT}/liquid_glue_test"
  g++ -std=c++20 -Wall -Wextra -Werror -Wshadow -O2 \
    -I"${LC_TREE}" \
    -o "${bin}" \
    "${LC_TREE}/modules/audio_coding/codecs/liquidcodec/liquid_glue_test.cc" \
    "${LC_TREE}/modules/audio_coding/codecs/liquidcodec/liquid_codec.cc" \
    "${LC_TREE}/modules/audio_coding/codecs/liquidcodec/liquid_stream.cc" \
    "${LC_TREE}/modules/audio_coding/codecs/liquidcodec/liquid_tail.cc"
  local log="${OUT}/stage2.log"
  if ! LIQUID_TEST_DIR="${OUT}" "${bin}" | tee "${log}"; then
    add_row stage=2 clip=codes codec=liquidcodec pack="" fpp="" status=fail score=0 score_kind=bit_exact detail=glue_test_failed
    return
  fi
  local count
  count="$(grep -c '^QUALITY_PACK ' "${log}" || true)"
  if [[ "${count}" != "6" ]]; then
    add_row stage=2 clip=codes codec=liquidcodec status=fail score=0 score_kind=bit_exact \
      detail="expected 6 pack rows, got ${count}"
    return
  fi
  local line pack fpp exact
  while IFS= read -r line; do
    pack="$(sed -n 's/.*pack=\([^ ]*\).*/\1/p' <<<"${line}")"
    fpp="$(sed -n 's/.*fpp=\([^ ]*\).*/\1/p' <<<"${line}")"
    exact="$(sed -n 's/.*bit_exact=\([^ ]*\).*/\1/p' <<<"${line}")"
    add_row stage=2 clip=codes codec=liquidcodec pack="${pack}" fpp="${fpp}" status=ok \
      score="${exact}" score_kind=bit_exact snr_db="" seg_snr_db="" lsd_db="" path=pack
  done < <(grep '^QUALITY_PACK ' "${log}")
}

stage_factory() {
  local stage="$1" codec="$2" compute="$3" encoder="$4" loss="$5" jitter="$6" opus="$7" clip="$8"
  local name
  name="$(basename "${clip}" .wav)"
  local dir="${OUT}/s${stage}-${codec}-${compute}-${encoder}-l${loss}-j${jitter}-b${opus}-${name}"
  if ! ensure_binary; then
    add_row stage="${stage}" clip="${name}" codec="${codec}" compute="${compute}" encoder="${encoder}" \
      loss_pct="${loss}" jitter_ms="${jitter}" opus_bps="${opus}" path=neteq status=skipped \
      score="" score_kind=seg_snr_db detail="directcall not built on this machine"
    return
  fi
  if [[ "${codec}" == "liquidcodec" ]]; then
    ensure_models || true
  fi
  local rc
  rc="$(run_quality_bin factory "${codec}" "${compute}" "${encoder}" "${loss}" "${jitter}" "${opus}" "${clip}" "${dir}")"
  local result status="fail"
  result="$(grep '^QUALITY_RESULT ' "${dir}/directcall.log" || true)"
  if [[ "${rc}" == "3" || "${result}" == *status=skipped* ]]; then
    status=skipped
  elif [[ "${rc}" == "0" ]]; then
    status=ok
  fi
  local bad clamped kbps algo jb
  bad="$(sed -n 's/.*out_of_codebook=\([^ ]*\).*/\1/p' <<<"${result}")"
  clamped="$(sed -n 's/.*clamped=\([^ ]*\).*/\1/p' <<<"${result}")"
  kbps="$(sed -n 's/.*onwire_kbps=\([^ ]*\).*/\1/p' <<<"${result}")"
  algo="$(sed -n 's/.*algo_ms=\([^ ]*\).*/\1/p' <<<"${result}")"
  jb="$(sed -n 's/.*jitter_buffer_ms=\([^ ]*\).*/\1/p' <<<"${result}")"
  local snr="" seg="" lsd="" pesq="" score="" kind="seg_snr_db"
  if [[ "${status}" == "ok" && -f "${dir}/quality_out.wav" ]]; then
    local line
    line="$(score_wav "${clip}" "${dir}/quality_out.wav" || true)"
    parse_score "${line}"
    snr="${SCORE_SNR}"
    seg="${SCORE_SEG}"
    lsd="${SCORE_LSD}"
    pesq="${SCORE_PESQ}"
    score="${seg}"
  fi
  local latency="${algo}"
  if [[ -n "${jb}" && -n "${algo}" ]]; then
    latency="$(awk -v a="${algo}" -v b="${jb}" 'BEGIN { printf "%.1f", a + b }')"
  fi
  add_row stage="${stage}" clip="${name}" codec="${codec}" compute="${compute}" encoder="${encoder}" \
    loss_pct="${loss}" jitter_ms="${jitter}" opus_bps="${opus}" path=neteq status="${status}" \
    score="${score}" score_kind="${kind}" snr_db="${snr}" seg_snr_db="${seg}" lsd_db="${lsd}" \
    pesq_wb="${pesq}" out_of_codebook="${bad}" clamped="${clamped}" onwire_kbps="${kbps}" \
    latency_ms="${latency}" detail="${result}"
}

stage1() {
  local computes=(cpu ane) encoders=(int8 fp16 fp32)
  if [[ "${QUICK}" == "1" ]]; then
    computes=(cpu)
    encoders=(int8)
  fi
  local clip compute encoder
  for clip in "${CLIPS[@]}"; do
    for compute in "${computes[@]}"; do
      for encoder in "${encoders[@]}"; do
        local name dir rc result
        name="$(basename "${clip}" .wav)"
        dir="${OUT}/s1-${compute}-${encoder}-${name}"
        if ! ensure_binary; then
          add_row stage=1 clip="${name}" codec=liquidcodec compute="${compute}" encoder="${encoder}" \
            path=offline status=skipped score="" score_kind=seg_snr_db detail="needs the macOS liquidcodec build"
          continue
        fi
        ensure_models || true
        rc="$(run_quality_bin offline liquidcodec "${compute}" "${encoder}" 0 0 0 "${clip}" "${dir}")"
        result="$(grep '^QUALITY_RESULT ' "${dir}/directcall.log" || true)"
        local status=fail
        if [[ "${rc}" == "3" || "${result}" == *status=skipped* ]]; then status=skipped
        elif [[ "${rc}" == "2" ]]; then status=skipped
        elif [[ "${rc}" == "0" ]]; then status=ok
        fi
        local bad clamped kbps algo snr="" seg="" lsd="" pesq="" score=""
        bad="$(sed -n 's/.*out_of_codebook=\([^ ]*\).*/\1/p' <<<"${result}")"
        clamped="$(sed -n 's/.*clamped=\([^ ]*\).*/\1/p' <<<"${result}")"
        kbps="$(sed -n 's/.*onwire_kbps=\([^ ]*\).*/\1/p' <<<"${result}")"
        algo="$(sed -n 's/.*algo_ms=\([^ ]*\).*/\1/p' <<<"${result}")"
        if [[ "${status}" == "ok" ]]; then
          local line
          line="$(score_wav "${clip}" "${dir}/quality_out.wav" || true)"
          parse_score "${line}"
          snr="${SCORE_SNR}"; seg="${SCORE_SEG}"; lsd="${SCORE_LSD}"; pesq="${SCORE_PESQ}"; score="${seg}"
        fi
        add_row stage=1 clip="${name}" codec=liquidcodec compute="${compute}" encoder="${encoder}" \
          path=offline status="${status}" score="${score}" score_kind=seg_snr_db \
          snr_db="${snr}" seg_snr_db="${seg}" lsd_db="${lsd}" pesq_wb="${pesq}" \
          out_of_codebook="${bad}" clamped="${clamped}" onwire_kbps="${kbps}" latency_ms="${algo}" \
          detail="${result}"
      done
    done
  done
}

stage3() {
  local compute="ane"
  if [[ "${QUICK}" == "1" ]]; then
    compute="cpu"
  fi
  local clip
  for clip in "${CLIPS[@]}"; do
    stage_factory 3 liquidcodec "${compute}" int8 0 0 0 "${clip}"
  done
}

live_call() {
  local stage="$1" codec="$2" aec="$3" loss="$4" jitter="$5" opus="$6" clip="$7"
  local name
  name="$(basename "${clip}" .wav)"
  local dir="${OUT}/s${stage}-live-${codec}-aec${aec}-l${loss}-j${jitter}-b${opus}-${name}"
  if ! ensure_binary; then
    add_row stage="${stage}" clip="${name}" codec="${codec}" aec="${aec}" loss_pct="${loss}" \
      jitter_ms="${jitter}" opus_bps="${opus}" path=directcall status=skipped score="" \
      score_kind=seg_snr_db detail="directcall not built on this machine"
    return
  fi
  if [[ "${codec}" == "liquidcodec" ]]; then
    ensure_models || true
  fi
  local runner="${SCRIPT_DIR}/run_local_call.sh"
  set +e
  "${runner}" --duration "${CALL_SECONDS}" --inject-wav "${clip}" --record-dir "${dir}" \
    --aec "${aec}" --audio-codec "${codec}" --opus-bitrate "${opus}" \
    --impair-loss "${loss}" --impair-jitter-ms "${jitter}" --allow-loss \
    --model-dir "${LC_MODELS}" --bin "${LC_BIN}"
  local rc=$?
  set -e
  local status=ok
  [[ "${rc}" -ne 0 ]] && status=fail
  local log="${dir}/caller.log"
  local kbps algo m2e
  kbps="$(grep -o 'send_ipv4_udp_rtp_kbps=[0-9.]*' "${log}" 2>/dev/null | tail -n 1 | cut -d= -f2 || true)"
  algo="$(grep -o 'algo_ms=[0-9.]*' "${log}" 2>/dev/null | tail -n 1 | cut -d= -f2 || true)"
  m2e="$(grep -o 'mouth_to_ear_est_ms=[0-9.]*' "${log}" 2>/dev/null | tail -n 1 | cut -d= -f2 || true)"
  local snr="" seg="" lsd="" pesq="" score=""
  local heard="${dir}/callee/output_48k.wav"
  if [[ -f "${heard}" ]]; then
    local line
    line="$(score_wav "${clip}" "${heard}" || true)"
    parse_score "${line}"
    snr="${SCORE_SNR}"; seg="${SCORE_SEG}"; lsd="${SCORE_LSD}"; pesq="${SCORE_PESQ}"; score="${seg}"
  else
    status=fail
  fi
  add_row stage="${stage}" clip="${name}" codec="${codec}" aec="${aec}" loss_pct="${loss}" \
    jitter_ms="${jitter}" opus_bps="${opus}" path=directcall status="${status}" \
    score="${score}" score_kind=seg_snr_db snr_db="${snr}" seg_snr_db="${seg}" lsd_db="${lsd}" \
    pesq_wb="${pesq}" onwire_kbps="${kbps}" latency_ms="${m2e:-${algo}}" detail="rc=${rc}"
}

stage4() {
  local clip="${CLIPS[0]}"
  live_call 4 liquidcodec off 0 0 0 "${clip}"
  if [[ "${QUICK}" != "1" ]]; then
    live_call 4 liquidcodec on 0 0 0 "${clip}"
  fi
}

stage5() {
  local losses=(0 1 3 5 10) jitters=(0 20 50)
  if [[ "${QUICK}" == "1" ]]; then
    losses=(0 5)
    jitters=(0 20)
  fi
  local clip="${CLIPS[0]}" loss jitter
  for loss in "${losses[@]}"; do
    for jitter in "${jitters[@]}"; do
      local compute="ane"
      if [[ "${QUICK}" == "1" ]]; then compute="cpu"; fi
      stage_factory 5 liquidcodec "${compute}" int8 "${loss}" "${jitter}" 0 "${clip}"
    done
  done
  if [[ "${QUICK}" != "1" ]]; then
    live_call 5 liquidcodec off 5 20 0 "${clip}"
  fi
}

stage6() {
  local rates=(6000 8000 12000 16000)
  if [[ "${QUICK}" == "1" ]]; then
    rates=(8000)
  fi
  local clip="${CLIPS[0]}" rate loss
  local losses=(0 5) 
  if [[ "${QUICK}" != "1" ]]; then
    losses=(0 1 3 5 10)
  fi
  for rate in "${rates[@]}"; do
    stage_factory 6 opus cpu "" 0 0 "${rate}" "${clip}"
    for loss in "${losses[@]}"; do
      stage_factory 6 opus cpu "" "${loss}" 0 "${rate}" "${clip}"
    done
    if [[ "${QUICK}" != "1" && "${rate}" == "8000" ]]; then
      live_call 6 opus off 0 0 "${rate}" "${clip}"
      live_call 6 opus on 0 0 "${rate}" "${clip}"
    fi
    if [[ "${QUICK}" == "1" ]]; then
      live_call 6 opus off 0 0 "${rate}" "${clip}"
    fi
  done
}

any_fail=0
want_stage 1 && stage1
want_stage 2 && stage2
want_stage 3 && stage3
want_stage 4 && stage4
want_stage 5 && stage5
want_stage 6 && stage6

"${PYBIN}" "${PY}" --summary "${CSV}" "${MD}"
echo "results ${CSV}"
echo "summary ${MD}"
if [[ -f "${CSV}" ]] && grep -q ',fail,' "${CSV}"; then
  echo "one or more stages failed; see ${MD}" >&2
  exit 1
fi
