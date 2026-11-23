#!/usr/bin/env bash
# Linux-side checks for the LiquidCall glue. Does not load Core ML or the model.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${ROOT}/scripts/liquidcall/out"
mkdir -p "${OUT}"
g++ -std=c++20 -Wall -Wextra -Werror -Wshadow -O2 \
  -I"${ROOT}" \
  -o "${OUT}/liquid_glue_test" \
  "${ROOT}/modules/audio_coding/codecs/liquidcall/liquid_glue_test.cc" \
  "${ROOT}/modules/audio_coding/codecs/liquidcall/liquid_codec.cc" \
  "${ROOT}/modules/audio_coding/codecs/liquidcall/liquid_stream.cc" \
  "${ROOT}/modules/audio_coding/codecs/liquidcall/liquid_tail.cc"
LIQUID_TEST_DIR="${OUT}" "${OUT}/liquid_glue_test" | tee "${OUT}/glue.txt"
grep -c '^QUALITY_PACK ' "${OUT}/glue.txt" | grep -qx 8
if ! pkg-config --exists opus; then
  echo "libopus development files are required (pkg-config opus, e.g. libopus-dev)" >&2
  exit 1
fi
g++ -std=c++20 -Wall -Wextra -Werror -Wshadow -O2 \
  -I"${ROOT}" \
  $(pkg-config --cflags opus) \
  -o "${OUT}/liquid_dual_codec_test" \
  "${ROOT}/modules/audio_coding/codecs/liquidcall/liquid_dual_codec_test.cc" \
  $(pkg-config --libs opus)
LIQUID_REPO_ROOT="${ROOT}" "${OUT}/liquid_dual_codec_test" | tee "${OUT}/dual_codec.txt"
grep -q 'liquidcall dual-codec tests passed' "${OUT}/dual_codec.txt"
python3 -m pip install --quiet numpy
python3 "${ROOT}/scripts/liquidcall/check_tail.py" "${OUT}/tail_case.bin"
python3 "${ROOT}/scripts/liquidcall/bandwidth.py" --self-check
python3 "${ROOT}/scripts/liquidcall/bandwidth.py" --theoretical --out "${OUT}/theoretical.csv"
bash -n "${ROOT}/scripts/liquidcall/fetch_models.sh"
REWRITE="$(mktemp -d)"
printf '%s\n' '{"encoder_model":"stream/lc_enc_step_int8.mlpackage","decoder_model":"stream/lc_dec_n1_int8.mlpackage"}' \
  > "${REWRITE}/liquidcall.json"
bash "${ROOT}/scripts/liquidcall/fetch_models.sh" --rewrite-manifest "${REWRITE}/liquidcall.json"
grep -q 'stream/lc_enc_step_int8.mlmodelc' "${REWRITE}/liquidcall.json"
grep -q 'stream/lc_dec_n1_int8.mlmodelc' "${REWRITE}/liquidcall.json"
if grep -q mlpackage "${REWRITE}/liquidcall.json"; then
  echo "manifest rewrite left a .mlpackage path" >&2
  exit 1
fi
rm -rf "${REWRITE}"
bash -n "${ROOT}/scripts/liquidcall/bench_bandwidth.sh"
bash -n "${ROOT}/scripts/liquidcall/run_local_call.sh"
bash -n "${ROOT}/scripts/liquidcall/quality_ladder.sh"
bash -n "${ROOT}/scripts/liquidcall/run_ios_quality.sh"
bash -n "${ROOT}/build.sh"
python3 "${ROOT}/scripts/liquidcall/quality_metrics.py" --self-check
python3 "${ROOT}/scripts/liquidcall/check_shadow.py"
python3 "${ROOT}/scripts/liquidcall/tone_wav.py" "${OUT}/tone.wav" 0.05
test -s "${OUT}/tone.wav"
PREP="$(mktemp -d)"
bash "${ROOT}/scripts/liquidcall/run_local_call.sh" --prepare-only --duration 2 --record-dir "${PREP}"
grep -q -- '--inject-wav=' "${PREP}/commands.txt"
grep -q -- '--record-dir=' "${PREP}/commands.txt"
grep -q '127.0.0.1:' "${PREP}/commands.txt"
grep -q -- '--liquid-model-dir=' "${PREP}/commands.txt"
test -s "${PREP}/inject.wav"
rm -rf "${PREP}"
READY="$(mktemp -d)/models"
mkdir -p "${READY}/stream/lc_enc_step_int8.mlmodelc" "${READY}/stream/lc_dec_n1_int8.mlmodelc"
mkdir -p "${READY}/stream/lc_enc_step_int8.mlpackage"
printf '%s\n' '{"encoder_model":"stream/lc_enc_step_int8.mlmodelc","decoder_model":"stream/lc_dec_n1_int8.mlmodelc"}' \
  > "${READY}/liquidcall.json"
echo keep > "${READY}/KEEP"
bash "${ROOT}/scripts/liquidcall/fetch_models.sh" --dest "${READY}" | tee "${OUT}/fetch-ready.txt"
grep -q 'models ready' "${OUT}/fetch-ready.txt"
test "$(cat "${READY}/KEEP")" = keep
if ! grep -q 'GN_BIN' "${ROOT}/build.sh"; then
  echo "build.sh does not pin gn" >&2
  exit 1
fi
grep -q 'third_party/depot_tools' "${ROOT}/build.sh"
grep -q 'fetch_models.sh' "${ROOT}/build.sh"
grep -q 'run_local_call.sh' "${ROOT}/build.sh"

# The macOS ninja build is not part of Linux CI. These are the mistakes that
# failed `ninja -C out/release directcall` on Apple clang and that a relative
# path in this directory hides (../../../ is //modules, not the repo root).
GN="${ROOT}/modules/audio_coding/codecs/liquidcall/BUILD.gn"
grep -q 'import("//webrtc.gni")' "${GN}"
grep -q 'rtc_library("liquidcall")' "${GN}"
if grep -nE '\.\./' "${GN}" | grep -vE '^[^:]+:[[:space:]]*#'; then
  echo "liquidcall BUILD.gn has a relative path; use //webrtc.gni and //api, //rtc_base, ..." >&2
  exit 1
fi
if grep -nE 'Factory final' \
    "${ROOT}/modules/audio_coding/codecs/liquidcall/liquid_factory.cc" \
    "${ROOT}/examples/direct/audio_tap.cc"; then
  echo "make_ref_counted derives from the factory, so it cannot be final" >&2
  exit 1
fi
for f in audio_decoder_liquid.cc audio_encoder_liquid.cc liquid_codec.cc \
         liquid_coreml.mm liquid_factory.cc liquid_stream.cc liquid_tail.cc; do
  if ! grep -q 'allow_unsafe_buffers' \
      "${ROOT}/modules/audio_coding/codecs/liquidcall/${f}"; then
    echo "missing UNSAFE_BUFFERS_BUILD pragma in ${f}" >&2
    exit 1
  fi
done
echo "glue tests and script syntax passed"
