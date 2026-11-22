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
echo "glue tests and script syntax passed"
