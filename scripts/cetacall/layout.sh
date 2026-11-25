# Shared locations for build.sh output. Source this; do not execute it.
# LC_TREE is the git checkout that contains this scripts/ directory.
# When that checkout is src/ under a gclient parent, models live in the
# parent (where ./build.sh and directcall are started) and the binary is
# src/out/release/directcall.
_LC_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LC_TREE="$(cd "${_LC_HERE}/../.." && pwd)"
if [[ "$(basename "${LC_TREE}")" == "src" && -e "${LC_TREE}/../.git" ]]; then
  LC_PARENT="$(cd "${LC_TREE}/.." && pwd)"
  LC_BIN="${LC_TREE}/out/release/directcall"
  LC_MODELS="${LC_PARENT}/scripts/cetacall/models"
elif [[ -d "${LC_TREE}/src/.git" || -d "${LC_TREE}/src/out" ]]; then
  LC_BIN="${LC_TREE}/src/out/release/directcall"
  LC_MODELS="${_LC_HERE}/models"
else
  LC_BIN="${LC_TREE}/out/release/directcall"
  LC_MODELS="${_LC_HERE}/models"
fi
