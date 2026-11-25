#!/usr/bin/env bash
# Linux-side checks for the CetaCall glue. Does not load Core ML or the model.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${ROOT}/scripts/cetacall/out"
mkdir -p "${OUT}"
g++ -std=c++20 -Wall -Wextra -Werror -Wshadow -O2 \
  -I"${ROOT}" \
  -o "${OUT}/cetacall_glue_test" \
  "${ROOT}/modules/audio_coding/codecs/cetacall/cetacall_glue_test.cc" \
  "${ROOT}/modules/audio_coding/codecs/cetacall/cetacall_codec.cc" \
  "${ROOT}/modules/audio_coding/codecs/cetacall/cetacall_stream.cc" \
  "${ROOT}/modules/audio_coding/codecs/cetacall/cetacall_tail.cc"
CETACALL_TEST_DIR="${OUT}" "${OUT}/cetacall_glue_test" | tee "${OUT}/glue.txt"
grep -c '^QUALITY_PACK ' "${OUT}/glue.txt" | grep -qx 8
if ! pkg-config --exists opus; then
  echo "libopus development files are required (pkg-config opus, e.g. libopus-dev)" >&2
  exit 1
fi
g++ -std=c++20 -Wall -Wextra -Werror -Wshadow -O2 \
  -I"${ROOT}" \
  $(pkg-config --cflags opus) \
  -o "${OUT}/cetacall_dual_codec_test" \
  "${ROOT}/modules/audio_coding/codecs/cetacall/cetacall_dual_codec_test.cc" \
  $(pkg-config --libs opus)
CETACALL_REPO_ROOT="${ROOT}" "${OUT}/cetacall_dual_codec_test" | tee "${OUT}/dual_codec.txt"
grep -q 'cetacall dual-codec tests passed' "${OUT}/dual_codec.txt"
python3 -m pip install --quiet numpy
python3 "${ROOT}/scripts/cetacall/check_tail.py" "${OUT}/tail_case.bin"
python3 "${ROOT}/scripts/cetacall/bandwidth.py" --self-check
python3 "${ROOT}/scripts/cetacall/bandwidth.py" --theoretical --out "${OUT}/theoretical.csv"
bash -n "${ROOT}/scripts/cetacall/fetch_models.sh"
REWRITE="$(mktemp -d)"
printf '%s\n' '{"encoder_model":"stream/lc_enc_step_int8.mlpackage","decoder_model":"stream/lc_dec_n1_int8.mlpackage"}' \
  > "${REWRITE}/cetacall.json"
bash "${ROOT}/scripts/cetacall/fetch_models.sh" --rewrite-manifest "${REWRITE}/cetacall.json"
grep -q 'stream/lc_enc_step_int8.mlmodelc' "${REWRITE}/cetacall.json"
grep -q 'stream/lc_dec_n1_int8.mlmodelc' "${REWRITE}/cetacall.json"
if grep -q mlpackage "${REWRITE}/cetacall.json"; then
  echo "manifest rewrite left a .mlpackage path" >&2
  exit 1
fi
rm -rf "${REWRITE}"
bash -n "${ROOT}/scripts/cetacall/bench_bandwidth.sh"
bash -n "${ROOT}/scripts/cetacall/run_local_call.sh"
bash -n "${ROOT}/scripts/cetacall/quality_ladder.sh"
bash -n "${ROOT}/scripts/cetacall/run_ios_quality.sh"
bash -n "${ROOT}/build.sh"
bash -n "${ROOT}/scripts/build_voiceswap_frameworks.sh"
grep -q 'OUT="${OUT:-out/ios_voiceswap}"' "${ROOT}/scripts/build_voiceswap_frameworks.sh"
if [[ "$(uname -s)" != "Darwin" ]]; then
  default_msg="$(bash "${ROOT}/scripts/build_voiceswap_frameworks.sh" 2>&1 || true)"
  grep -q 'iOS frameworks output: out/ios_voiceswap' <<<"${default_msg}"
  override_msg="$(OUT=out/ios_dualcodec bash "${ROOT}/scripts/build_voiceswap_frameworks.sh" 2>&1 || true)"
  grep -q 'iOS frameworks output: out/ios_dualcodec' <<<"${override_msg}"
  flag_msg="$(bash "${ROOT}/scripts/build_voiceswap_frameworks.sh" --out=ios_dualcodec 2>&1 || true)"
  grep -q 'iOS frameworks output: out/ios_dualcodec' <<<"${flag_msg}"
  help_msg="$(bash "${ROOT}/scripts/build_voiceswap_frameworks.sh" --help)"
  grep -q 'out/ios_voiceswap' <<<"${help_msg}"
fi
python3 "${ROOT}/scripts/cetacall/quality_metrics.py" --self-check
python3 "${ROOT}/scripts/cetacall/check_shadow.py"
python3 "${ROOT}/scripts/cetacall/tone_wav.py" "${OUT}/tone.wav" 0.05
test -s "${OUT}/tone.wav"
PREP="$(mktemp -d)"
bash "${ROOT}/scripts/cetacall/run_local_call.sh" --prepare-only --duration 2 --record-dir "${PREP}"
grep -q -- '--inject-wav=' "${PREP}/commands.txt"
grep -q -- '--record-dir=' "${PREP}/commands.txt"
grep -q '127.0.0.1:' "${PREP}/commands.txt"
grep -q -- '--cetacall-model-dir=' "${PREP}/commands.txt"
test -s "${PREP}/inject.wav"
rm -rf "${PREP}"
READY="$(mktemp -d)/models"
mkdir -p "${READY}/stream/lc_enc_step_int8.mlmodelc" "${READY}/stream/lc_dec_n1_int8.mlmodelc"
mkdir -p "${READY}/stream/lc_enc_step_int8.mlpackage"
printf '%s\n' '{"encoder_model":"stream/lc_enc_step_int8.mlmodelc","decoder_model":"stream/lc_dec_n1_int8.mlmodelc"}' \
  > "${READY}/cetacall.json"
echo keep > "${READY}/KEEP"
bash "${ROOT}/scripts/cetacall/fetch_models.sh" --dest "${READY}" | tee "${OUT}/fetch-ready.txt"
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
GN="${ROOT}/modules/audio_coding/codecs/cetacall/BUILD.gn"
grep -q 'import("//webrtc.gni")' "${GN}"
grep -q 'rtc_library("cetacall")' "${GN}"
if grep -nE '\.\./' "${GN}" | grep -vE '^[^:]+:[[:space:]]*#'; then
  echo "cetacall BUILD.gn has a relative path; use //webrtc.gni and //api, //rtc_base, ..." >&2
  exit 1
fi
if grep -nE 'Factory final' \
    "${ROOT}/modules/audio_coding/codecs/cetacall/cetacall_factory.cc" \
    "${ROOT}/examples/direct/audio_tap.cc"; then
  echo "make_ref_counted derives from the factory, so it cannot be final" >&2
  exit 1
fi
for f in audio_decoder_cetacall.cc audio_encoder_cetacall.cc cetacall_codec.cc \
         cetacall_coreml.mm cetacall_factory.cc cetacall_stream.cc cetacall_tail.cc; do
  if ! grep -q 'allow_unsafe_buffers' \
      "${ROOT}/modules/audio_coding/codecs/cetacall/${f}"; then
    echo "missing UNSAFE_BUFFERS_BUILD pragma in ${f}" >&2
    exit 1
  fi
done
echo "glue tests and script syntax passed"
