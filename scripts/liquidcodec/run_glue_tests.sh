#!/usr/bin/env bash
# Linux-side checks for the LiquidCodec glue. Does not load Core ML or the model.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${ROOT}/scripts/liquidcodec/out"
mkdir -p "${OUT}"
g++ -std=c++20 -Wall -Wextra -Werror -O2 \
  -I"${ROOT}" \
  -o "${OUT}/liquid_glue_test" \
  "${ROOT}/modules/audio_coding/codecs/liquidcodec/liquid_glue_test.cc" \
  "${ROOT}/modules/audio_coding/codecs/liquidcodec/liquid_codec.cc" \
  "${ROOT}/modules/audio_coding/codecs/liquidcodec/liquid_stream.cc" \
  "${ROOT}/modules/audio_coding/codecs/liquidcodec/liquid_tail.cc"
LIQUID_TEST_DIR="${OUT}" "${OUT}/liquid_glue_test"
python3 -m pip install --quiet numpy
python3 "${ROOT}/scripts/liquidcodec/check_tail.py" "${OUT}/tail_case.bin"
python3 "${ROOT}/scripts/liquidcodec/bandwidth.py" --self-check
python3 "${ROOT}/scripts/liquidcodec/bandwidth.py" --theoretical --out "${OUT}/theoretical.csv"
bash -n "${ROOT}/scripts/liquidcodec/fetch_models.sh"
bash -n "${ROOT}/scripts/liquidcodec/bench_bandwidth.sh"
bash -n "${ROOT}/build.sh"

# The macOS ninja build is not part of Linux CI. These are the mistakes that
# failed `ninja -C out/release directcall` on Apple clang and that a relative
# path in this directory hides (../../../ is //modules, not the repo root).
GN="${ROOT}/modules/audio_coding/codecs/liquidcodec/BUILD.gn"
grep -q 'import("//webrtc.gni")' "${GN}"
grep -q 'rtc_library("liquidcodec")' "${GN}"
if grep -nE '\.\./' "${GN}" | grep -vE '^[^:]+:[[:space:]]*#'; then
  echo "liquidcodec BUILD.gn has a relative path; use //webrtc.gni and //api, //rtc_base, ..." >&2
  exit 1
fi
if grep -nE 'Factory final' \
    "${ROOT}/modules/audio_coding/codecs/liquidcodec/liquid_factory.cc" \
    "${ROOT}/examples/direct/audio_tap.cc"; then
  echo "make_ref_counted derives from the factory, so it cannot be final" >&2
  exit 1
fi
for f in audio_decoder_liquid.cc audio_encoder_liquid.cc liquid_codec.cc \
         liquid_coreml.mm liquid_factory.cc liquid_stream.cc liquid_tail.cc; do
  if ! grep -q 'allow_unsafe_buffers' \
      "${ROOT}/modules/audio_coding/codecs/liquidcodec/${f}"; then
    echo "missing UNSAFE_BUFFERS_BUILD pragma in ${f}" >&2
    exit 1
  fi
done
echo "glue tests and script syntax passed"
