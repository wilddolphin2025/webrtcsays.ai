#!/bin/bash

# Exit on any error
set -e

# Always set repo root ONCE at the top; all relative paths assume repo root.
REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$REPO_ROOT"

# Pull out -b / --branch (and optional --branch=ref) so remaining args match the
# build-type parser below. Outer repo still follows its current branch; src/ uses SRC_GIT_REF.
# Optional --whillats-accel overrides env WHILLATS_ACCEL for whillats (GGML) backend.
SRC_BRANCH_OVERRIDE=""
WHILLATS_ACCEL_CLI=""
EARLY_ARGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    -b|--branch)
      if [ -z "${2:-}" ]; then
        echo "ERROR: $1 requires a branch name, tag, or commit"
        exit 1
      fi
      SRC_BRANCH_OVERRIDE="$2"
      shift 2
      ;;
    --branch=*)
      SRC_BRANCH_OVERRIDE="${1#*=}"
      if [ -z "$SRC_BRANCH_OVERRIDE" ]; then
        echo "ERROR: --branch= requires a non-empty ref"
        exit 1
      fi
      shift
      ;;
    --whillats-accel)
      if [ -z "${2:-}" ]; then
        echo "ERROR: --whillats-accel requires a value: cpu, cuda, metal, or auto"
        exit 1
      fi
      WHILLATS_ACCEL_CLI="$2"
      shift 2
      ;;
    --whillats-accel=*)
      WHILLATS_ACCEL_CLI="${1#*=}"
      if [ -z "$WHILLATS_ACCEL_CLI" ]; then
        echo "ERROR: --whillats-accel= requires a non-empty value"
        exit 1
      fi
      shift
      ;;
    *)
      EARLY_ARGS+=("$1")
      shift
      ;;
  esac
done
if [ ${#EARLY_ARGS[@]} -gt 0 ]; then
  set -- "${EARLY_ARGS[@]}"
else
  set --
fi

# WebRTC top-level directories that may appear at the gclient parent alongside
# `src/` as stray checkouts. Remove only when nothing under them is tracked in
# git (avoids deleting the real working tree or folders like models/).
WEBRTC_SPILL_DIRNAMES=(
  api audio base build build_overrides buildtools call common_audio common_video
  data docs examples experiments g3doc infra ios logging media modules mojo net
  p2p pc resources rtc_base rtc_tools sdk stats system_wrappers test testing
  third_party tools tools_webrtc video out
)

remove_webrtc_spill_at_root() {
  local d
  for d in "${WEBRTC_SPILL_DIRNAMES[@]}"; do
    [ -d "$REPO_ROOT/$d" ] || continue
    echo "Removing WebRTC spill directory at repo root (duplicate of src/): $d"
    rm -rf "$REPO_ROOT/$d"
  done
}

CURRENT_BRANCH=$(git rev-parse --abbrev-ref HEAD)
CURRENT_SHA=$(git rev-parse HEAD)
if [ "$CURRENT_BRANCH" = "HEAD" ]; then
  echo "Detached HEAD detected (tag build). Using commit SHA: $CURRENT_SHA"
  CURRENT_BRANCH="$CURRENT_SHA"
fi

# Ref for src/ and whillats: explicit -b/--branch, else SRC_BRANCH env, else outer branch.
if [ -n "$SRC_BRANCH_OVERRIDE" ]; then
  SRC_GIT_REF="$SRC_BRANCH_OVERRIDE"
elif [ -n "${SRC_BRANCH:-}" ]; then
  SRC_GIT_REF="$SRC_BRANCH"
else
  SRC_GIT_REF="$CURRENT_BRANCH"
fi
if [ -n "$SRC_BRANCH_OVERRIDE" ] || [ -n "${SRC_BRANCH:-}" ]; then
  echo "src (and whillats) will checkout: $SRC_GIT_REF"
fi

WHILLATS_DIR="$REPO_ROOT/src/modules/third_party/whillats"

# Clean up old directories if they exist, but keep src
echo "Cleaning up old directories..."
if [ -f ".gclient" ]; then
    echo "Removing existing .gclient file..."
    rm -f .gclient
fi

# Ensure we're on the correct branch/commit
echo "Switching to $CURRENT_BRANCH..."
git fetch origin
git checkout "$CURRENT_BRANCH"

# Verify we're on the correct branch
echo "Current commit: $(git rev-parse HEAD)"
echo "Current branch: $(git branch --show-current)"

# Store the current commit hash for comparison
CURRENT_COMMIT=$(git rev-parse HEAD)
LAST_BUILD_COMMIT_FILE="last_build_commit.txt"
ORIGIN_URL=$(git remote get-url origin)

# Create or update .gclient file in the root directory with 'name': 'src'
cat > .gclient << EOF
solutions = [
  {
    "managed": False,
    "name": "src",
    "url": "$ORIGIN_URL",
    "custom_deps": {},
    "custom_vars": {
      "checkout_instrumented_libraries": False,
      "checkout_android": False,
      "checkout_ios": $( [ "${1:-}" = "ios" ] && echo True || echo False ),
    },
    "deps_file": "DEPS",
  },
]
EOF

# Set Python to 3.12 for depot_tools compatibility (3.13 has issues)
if command -v python3.12 >/dev/null 2>&1; then
    export PYTHON_BIN_PATH=$(which python3.12)
    export DEPOT_TOOLS_PYTHON3_PATH=$(which python3.12)
    echo "Using Python 3.12 for depot_tools compatibility"
elif command -v python3.11 >/dev/null 2>&1; then
    export PYTHON_BIN_PATH=$(which python3.11)
    export DEPOT_TOOLS_PYTHON3_PATH=$(which python3.11)
    echo "Using Python 3.11 for depot_tools compatibility"
else
    echo "Warning: depot_tools may have compatibility issues with Python 3.13+"
fi

# Prepend depot_tools when present. Ninja/gn invoke vpython3 from here; if gclient
# exists elsewhere on PATH we must still prepend or builds fail with "vpython3: not found".
DEPOT_TOOLS_DIR=""
if [ -d "$HOME/depot_tools" ]; then
    DEPOT_TOOLS_DIR="$HOME/depot_tools"
elif [ -d "$HOME/Public/depot_tools" ]; then
    DEPOT_TOOLS_DIR="$HOME/Public/depot_tools"
fi
if [ -n "$DEPOT_TOOLS_DIR" ]; then
    case ":$PATH:" in
        *":$DEPOT_TOOLS_DIR:"*) echo "depot_tools already at front of PATH";;
        *)
            PATH="$DEPOT_TOOLS_DIR:$PATH"
            echo "Prepended $DEPOT_TOOLS_DIR to PATH (gclient, vpython3, gn, ...)"
            ;;
    esac
elif ! command -v gclient >/dev/null 2>&1; then
    echo "depot_tools not found in PATH or expected directories."
    echo "Attempting to install depot_tools..."
    git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git "$HOME/depot_tools"
    if [ $? -eq 0 ]; then
        PATH="$HOME/depot_tools:$PATH"
        echo "Successfully installed and added depot_tools to PATH"
    else
        echo "ERROR: Failed to install depot_tools. Please install depot_tools manually."
        exit 1
    fi
fi

if ! command -v vpython3 >/dev/null 2>&1; then
    echo "ERROR: vpython3 not found. Chromium ninja needs depot_tools on PATH."
    echo "  export PATH=\"\$HOME/depot_tools:\$PATH\"   # then: cd src && ninja -C out/debug directcall"
    exit 1
fi


# First run: let gclient clone src/. Then check out the requested ref *before*
# the real sync so that ref's DEPS decides the toolchain, sysroot and
# third_party revisions (syncing first left M134 build files under an M156 tree).
if [ ! -e "src/.git" ]; then
    echo "Cloning src/ ..."
    gclient sync --nohooks --no-history --shallow
fi
echo "Checking out $SRC_GIT_REF in src/ ..."
git -C src fetch --depth=1 origin "$SRC_GIT_REF" ||
    { echo "ERROR: cannot fetch $SRC_GIT_REF"; exit 1; }
git -C src checkout -B "$SRC_GIT_REF" FETCH_HEAD 2>/dev/null ||
    git -C src checkout --detach FETCH_HEAD
# whillats follows the commit pinned by src/, including its engines.
git -C src submodule update --init --recursive --depth=1 modules/third_party/whillats

echo "Syncing dependencies for $(git -C src rev-parse --short HEAD) ..."
gclient sync -D --force --nohooks --no-history --shallow -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
# Hooks, gn and ninja must use the depot_tools this DEPS pins: an older
# ~/depot_tools cannot parse newer vpython.toml files ("requires-python").
if [ -x "src/third_party/depot_tools/gclient" ]; then
    PATH="$REPO_ROOT/src/third_party/depot_tools:$PATH"
    echo "Using pinned depot_tools from src/third_party/depot_tools"
    # DEPOT_TOOLS_UPDATE=0 skips this, and gn then fails with
    # "python3_bin_reldir.txt not found".
    if [ ! -f "src/third_party/depot_tools/python3_bin_reldir.txt" ]; then
        echo "Bootstrapping pinned depot_tools ..."
        ( cd src/third_party/depot_tools && DEPOT_TOOLS_UPDATE=1 ./ensure_bootstrap )
    fi
    export DEPOT_TOOLS_UPDATE=0
fi
gclient runhooks
if [ ! -f "src/build/dotfile_settings.gni" ] ||
   ! grep -q expand_directory_allowlist src/build/dotfile_settings.gni; then
    echo "ERROR: src/build does not match src/DEPS after gclient sync."
    exit 1
fi

# After sync, drop previous Chromium ninja output under src/ (often tens of GB). gn/ninja
# recreates out/. Set BUILD_KEEP_SRC_OUT=1 to preserve an existing src/out tree.
if [ "${BUILD_KEEP_SRC_OUT:-}" != "1" ] && [ -d "$REPO_ROOT/src/out" ]; then
    echo "Removing $REPO_ROOT/src/out (previous build output) to free disk space before configuring..."
    rm -rf "$REPO_ROOT/src/out"
fi

# Drop stray duplicate tree at gclient root whether or not we synced this run.
remove_webrtc_spill_at_root

# Optional: remove root .cipd cache (large; recreated on next sync). Off by default.
if [ "${BUILD_RM_ROOT_CIPD:-}" = "1" ] && [ -d "$REPO_ROOT/.cipd" ]; then
    echo "BUILD_RM_ROOT_CIPD=1: removing $REPO_ROOT/.cipd"
    rm -rf "$REPO_ROOT/.cipd"
fi

# Copy .vpython3 to src directory
cp .vpython3 src/ || { echo "WARNING: .vpython3 not found in root, build may fail if dependencies are incorrect."; }

# Navigate to src directory
cd src

# Detect platform architecture for sysroot
ARCH=$(uname -m)
case "$ARCH" in
    x86_64)
        SYSROOT_ARCH=amd64
        ;;
    aarch64|arm64)
        SYSROOT_ARCH=arm64
        ;;
    *)
        echo "Unsupported architecture: $ARCH"
        SYSROOT_ARCH=""
        ;;
esac


# For ARM64 Linux, use system Clang and ensure required packages are installed
if [[ "$OSTYPE" != "darwin"* ]] && ([ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]); then
    echo "Detected ARM64 Linux platform. Using system Clang and ensuring required packages are installed..."
    apt-get update
    apt-get install -y clang lld || {
        echo "ERROR: Failed to install clang/lld. Please check your APT sources and keys.";
        exit 1;
    }
    CLANG_PATH=$(which clang || true)
    if [ -z "$CLANG_PATH" ]; then
        echo "ERROR: clang not found after installation. Please install clang manually."
        exit 1
    fi
    echo "System Clang found at $CLANG_PATH"
    # Set GN args to use system Clang and specify clang_version 14
    EXTRA_ARGS="clang_base_path=\"/usr\" clang_use_chrome_plugins=false target_cpu=\"arm64\" use_custom_libcxx=false clang_version=\"14\""
    echo "Skipping prebuilt Clang download for ARM64 Linux."
    # Build and install libclang_rt.builtins.a if missing
    BUILTINS_PATH="/usr/lib/clang/14/lib/aarch64-unknown-linux-gnu/libclang_rt.builtins.a"
    if [ ! -f "$BUILTINS_PATH" ]; then
        echo "libclang_rt.builtins.a not found, building from source..."
        TMP_LLVM_DIR="/tmp/llvm-project"
        if [ ! -d "$TMP_LLVM_DIR" ]; then
            git clone --depth=1 https://github.com/llvm/llvm-project.git "$TMP_LLVM_DIR"
        fi
        cd "$TMP_LLVM_DIR"
        rm -rf build-builtin
        mkdir build-builtin
        cd build-builtin
        cmake -G Ninja \
          -DCMAKE_BUILD_TYPE=Release \
          -DLLVM_ENABLE_PROJECTS="compiler-rt" \
          -DLLVM_TARGETS_TO_BUILD="AArch64" \
          -DCOMPILER_RT_BUILD_BUILTINS=ON \
          -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON \
          -DCMAKE_C_COMPILER_TARGET=aarch64-linux-gnu \
          -DLLVM_INCLUDE_TESTS=OFF \
          -DCMAKE_INSTALL_PREFIX=/usr \
          ../llvm
        ninja
        FOUND_BUILTIN=$(find . -name 'libclang_rt.builtins.a' | head -n1)
        if [ -f "$FOUND_BUILTIN" ]; then
            sudo mkdir -p /usr/lib/clang/14/lib/aarch64-unknown-linux-gnu/
            sudo cp "$FOUND_BUILTIN" /usr/lib/clang/14/lib/aarch64-unknown-linux-gnu/
            echo "libclang_rt.builtins.a installed to /usr/lib/clang/14/lib/aarch64-unknown-linux-gnu/"
        else
            echo "ERROR: libclang_rt.builtins.a was not built successfully."
            exit 1
        fi
        cd -
    fi
fi

# For macOS, set deployment target and handle ARM64 if applicable
if [[ "$OSTYPE" == "darwin"* ]]; then
    echo "Detected macOS platform. Setting deployment target to 15.0..."
    if [ "$ARCH" = "arm64" ]; then
        echo "Detected macOS ARM64 (Apple Silicon). Combining ARM64 and macOS settings..."
        EXTRA_ARGS="mac_deployment_target=\"15.0\" target_cpu=\"arm64\""
    else
        echo "Detected macOS x86_64 (Intel). Using macOS settings only..."
        EXTRA_ARGS="mac_deployment_target=\"15.0\""
    fi
fi

build_whillats() {
    BUILD_TYPE="$1"
    echo "[build-whillats] Build type: $BUILD_TYPE"
    echo "[build-whillats] REPO_ROOT: $REPO_ROOT"
    WHILLATS_DIR="$REPO_ROOT/src/modules/third_party/whillats"
    echo "[build-whillats] WHILLATS_DIR: $WHILLATS_DIR"
    WHILLATS_THIRD_PARTY_DIR="$WHILLATS_DIR/third_party"
    echo "[build-whillats] WHILLATS_THIRD_PARTY_DIR: $WHILLATS_THIRD_PARTY_DIR"
    if ! grep -q 'modules/third_party/whillats' "$REPO_ROOT/.gitmodules"; then
        echo "ERROR: Submodule 'modules/third_party/whillats' is missing from .gitmodules."
        exit 1
    fi
    echo "[build-whillats] Current root .gitmodules contents:"
    cat "$REPO_ROOT/.gitmodules"
    # Use the whillats commit pinned by src/ (and its pinned engines);
    # WHILLATS_REF=<branch|tag> overrides it for experiments.
    if [ -n "${WHILLATS_REF:-}" ]; then
        echo "[build-whillats] WHILLATS_REF=$WHILLATS_REF overrides the pinned commit"
        git -C "$WHILLATS_DIR" fetch --depth=1 origin "$WHILLATS_REF" &&
            git -C "$WHILLATS_DIR" checkout --detach FETCH_HEAD &&
            git -C "$WHILLATS_DIR" submodule update --init --recursive --depth=1
    else
        git submodule update --init --recursive --depth=1 modules/third_party/whillats
    fi
    echo "[build-whillats] whillats at $(git -C "$WHILLATS_DIR" describe --tags --always)"

    if [ ! -d "$WHILLATS_DIR" ]; then
        echo "ERROR: $WHILLATS_DIR directory not found."
        exit 1
    fi
    
    # Check if CMakeLists.txt exists in the whillats directory
    if [ ! -f "$WHILLATS_DIR/CMakeLists.txt" ]; then
        echo "ERROR: CMakeLists.txt not found in $WHILLATS_DIR"
        echo "Contents of $WHILLATS_DIR:"
        ls -la "$WHILLATS_DIR"
        echo "Re-initializing submodule..."
        git submodule deinit -f modules/third_party/whillats || true
        git submodule update --init --recursive modules/third_party/whillats || true
        if [ ! -f "$WHILLATS_DIR/CMakeLists.txt" ]; then
            echo "ERROR: Still no CMakeLists.txt after re-initialization. Submodule may be corrupted."
            exit 1
        fi
    fi

    pushd "$WHILLATS_DIR"

    # --- Whillats GGML backend: default CPU; opt into CUDA or Metal ---
    # WHILLATS_ACCEL: cpu (default), cuda, metal (macOS), auto (legacy: Metal on mac, else CUDA if nvcc)
    # Optional: WHILLATS_CUDA_ARCH for cuda (e.g. 80-real); else use nvidia-smi compute_cap when available.
    local_accel="${WHILLATS_ACCEL_CLI:-${WHILLATS_ACCEL:-cpu}}"
    local_accel="$(printf '%s' "$local_accel" | tr '[:upper:]' '[:lower:]')"
    GPU_FLAGS=""
    case "$local_accel" in
      cpu)
        echo "[build-whillats] WHILLATS_ACCEL=cpu (default): CPU-only (no Metal, no CUDA)"
        GPU_FLAGS="-DGGML_METAL=OFF -DGGML_CUDA=OFF"
        ;;
      cuda)
        if ! command -v nvcc >/dev/null 2>&1; then
            echo "ERROR: WHILLATS_ACCEL=cuda but nvcc not found in PATH"
            exit 1
        fi
        echo "[build-whillats] WHILLATS_ACCEL=cuda: enabling CUDA"
        GPU_FLAGS="-DGGML_METAL=OFF -DGGML_CUDA=ON"
        if [ -n "${WHILLATS_CUDA_ARCH:-}" ]; then
            echo "[build-whillats] Using WHILLATS_CUDA_ARCH=${WHILLATS_CUDA_ARCH}"
            GPU_FLAGS="$GPU_FLAGS -DCMAKE_CUDA_ARCHITECTURES=${WHILLATS_CUDA_ARCH}"
        else
            NATIVE_ARCH=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -1 | tr -d '.')
            if [ -n "$NATIVE_ARCH" ]; then
                echo "[build-whillats] Detected native CUDA architecture: sm_${NATIVE_ARCH}"
                GPU_FLAGS="$GPU_FLAGS -DCMAKE_CUDA_ARCHITECTURES=${NATIVE_ARCH}-real"
            else
                echo "[build-whillats] WARNING: nvidia-smi not available; omitting CMAKE_CUDA_ARCHITECTURES (toolkit default)"
            fi
        fi
        ;;
      metal)
        if [[ "$OSTYPE" != "darwin"* ]]; then
            echo "[build-whillats] WARNING: WHILLATS_ACCEL=metal on non-macOS; using CPU-only"
            GPU_FLAGS="-DGGML_METAL=OFF -DGGML_CUDA=OFF"
        else
            echo "[build-whillats] WHILLATS_ACCEL=metal: enabling Metal"
            GPU_FLAGS="-DGGML_METAL=ON -DGGML_CUDA=OFF"
        fi
        ;;
      auto)
        echo "[build-whillats] WHILLATS_ACCEL=auto: legacy detection (Metal on macOS, else CUDA if nvcc, else CPU)"
        if [[ "$OSTYPE" == "darwin"* ]]; then
            echo "[build-whillats] macOS: Metal on"
            GPU_FLAGS="-DGGML_METAL=ON -DGGML_CUDA=OFF"
        elif command -v nvcc >/dev/null 2>&1; then
            echo "[build-whillats] NVCC found: CUDA on"
            GPU_FLAGS="-DGGML_METAL=OFF -DGGML_CUDA=ON"
            if [ -n "${WHILLATS_CUDA_ARCH:-}" ]; then
                GPU_FLAGS="$GPU_FLAGS -DCMAKE_CUDA_ARCHITECTURES=${WHILLATS_CUDA_ARCH}"
            else
                NATIVE_ARCH=$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -1 | tr -d '.')
                if [ -n "$NATIVE_ARCH" ]; then
                    echo "[build-whillats] Detected native CUDA architecture: sm_${NATIVE_ARCH}"
                    GPU_FLAGS="$GPU_FLAGS -DCMAKE_CUDA_ARCHITECTURES=${NATIVE_ARCH}-real"
                fi
            fi
        else
            echo "[build-whillats] No GPU toolchain: CPU-only"
            GPU_FLAGS="-DGGML_METAL=OFF -DGGML_CUDA=OFF"
        fi
        ;;
      *)
        echo "ERROR: Unknown WHILLATS_ACCEL='$local_accel' (use cpu, cuda, metal, or auto)"
        exit 1
        ;;
    esac
    # Check available disk space and memory before building
    AVAILABLE_SPACE=$(df . | tail -1 | awk '{print $4}')
    echo "[build-whillats] Available disk space: ${AVAILABLE_SPACE}KB"
    if [ "$AVAILABLE_SPACE" -lt 10485760 ]; then  # Less than 10GB
        echo "[build-whillats] WARNING: Less than 10GB available disk space. Build may fail."
    fi
    
    # Check available memory and adjust build strategy  
    # Handle different environments (native vs containerized)
    if [ -f "/.dockerenv" ] || [ -n "${GITHUB_ACTIONS}" ]; then
        # In containerized environment, be more conservative with memory estimates
        TOTAL_MEM=$(free -m | awk 'NR==2{printf "%.0f", $2}')
        AVAILABLE_MEM=$(echo "$TOTAL_MEM * 0.6" | bc 2>/dev/null || echo "2000")
        echo "[build-whillats] Container/CI environment detected"
        echo "[build-whillats] Total memory: ${TOTAL_MEM}MB, Conservative available: ${AVAILABLE_MEM}MB"
    else
        AVAILABLE_MEM=$(free -m | awk 'NR==2{printf "%.0f", $7}')
        echo "[build-whillats] Native environment, Available memory: ${AVAILABLE_MEM}MB"
    fi
    
    PARALLEL_JOBS=1
    if [ "$AVAILABLE_MEM" -lt 2000 ]; then
        echo "[build-whillats] Very low memory (<2GB), skipping whillats build"
        popd
        return 0
    elif [ "$AVAILABLE_MEM" -lt 4000 ]; then
        PARALLEL_JOBS=1
    elif [ "$AVAILABLE_MEM" -lt 8000 ]; then
        PARALLEL_JOBS=2
    else
        PARALLEL_JOBS=4
    fi
    echo "[build-whillats] Using $PARALLEL_JOBS parallel jobs"
    
    if [ -d "build" ]; then
        echo "[build-whillats] Removing previous build directory..."
        rm -rf build
    fi
    
    if [ "$BUILD_TYPE" = "debug" ]; then
        CMAKE_BUILD_TYPE="Debug"
    else
        CMAKE_BUILD_TYPE="Release"
    fi

    echo "[build-whillats] Configuring CMake ($CMAKE_BUILD_TYPE) with GPU flags: $GPU_FLAGS"
    # No --fresh: requires CMake 3.24+; build/ is removed above so configure is always clean.
    # Production TTS: Piper, plus Pocket TTS for personalized voices.
    # WHILLATS_TTS_FLAGS="-DWHILLATS_STYLETTS2=ON" etc. picks other engines.
    TTS_FLAGS="${WHILLATS_TTS_FLAGS:--DWHILLATS_PIPER=ON -DWHILLATS_POCKET_TTS=ON}"
    echo "[build-whillats] TTS flags: $TTS_FLAGS"
    # pocket-tts-raven fetches dr_libs with GIT_SHALLOW and a commit pin.
    # A shallow clone only has the default-branch tip, so the pin is missing.
    perl -0pi -e 's/(GIT_TAG cd99e2cca5ccb48f8d000f1cd844424dcdbbaf59 # dr_libs pinned\n)\s*GIT_SHALLOW TRUE\n/$1/'         "$WHILLATS_DIR/third_party/pocket-tts-raven/CMakeLists.txt" 2>/dev/null || true
    timeout 1200 cmake -S . -B build -DCMAKE_BUILD_TYPE=$CMAKE_BUILD_TYPE $GPU_FLAGS $TTS_FLAGS || {
        echo "[build-whillats] CMake configuration failed. Retrying with verbose output..."
        cmake -S . -B build -DCMAKE_BUILD_TYPE=$CMAKE_BUILD_TYPE $GPU_FLAGS $TTS_FLAGS -DCMAKE_VERBOSE_MAKEFILE=ON
    }
    echo "[build-whillats] Building with $PARALLEL_JOBS parallel jobs..."
    cmake --build build --parallel $PARALLEL_JOBS
    popd
    echo "[build-whillats] Whisper/Llama/TTS build completed."
}

# Dynamically detect src directory
if [ -d "src" ]; then
    SRC_DIR="src"
else
    SRC_DIR="."
fi

# Parse script parameters
BUILD_TYPE="debug"
ENABLE_WHILLATS="false"
IOS_BUILD="false"
if [ $# -ge 1 ]; then
    if [[ "$1" == "debug" || "$1" == "release" ]]; then
        BUILD_TYPE="$1"
    elif [[ "$1" == "ios" ]]; then
        IOS_BUILD="true"
        BUILD_TYPE="debug"  # Default to debug for iOS, can be overridden
    else
        echo "Usage: $0 [-b REF | --branch REF | --branch=REF] [build_type] [options]"
        echo ""
        echo "src/ checkout (whillats follows the commit pinned by src/; WHILLATS_REF overrides):"
        echo "  -b REF, --branch REF   Branch, tag, or commit for src (outer repo unchanged)"
        echo "  --branch=REF           Same as --branch"
        echo "  Env SRC_BRANCH=REF     Same if no -b/--branch on the command line"
        echo ""
        echo "Whillats GGML backend (host builds only; default is CPU):"
        echo "  --whillats-accel MODE   cpu | cuda | metal | auto (legacy detect)"
        echo "  Env WHILLATS_ACCEL=MODE Same if no --whillats-accel (default: cpu)"
        echo "  Env WHILLATS_CUDA_ARCH  Optional for cuda/auto, e.g. 80-real"
        echo ""
        echo "Build types:"
        echo "  debug       Build debug version for host platform"
        echo "  release     Build release version for host platform" 
        echo "  ios         Build for iOS (default: debug)"
        echo ""
        echo "Options:"
        echo "  whillats    Enable whillats speech/AI features"
        echo ""
        echo "iOS-specific usage:"
        echo "  $0 [-b REF] ios [debug|release] [whillats]"
        echo ""
        echo "Examples:"
        echo "  $0 -b talkingface release whillats"
        echo "  WHILLATS_ACCEL=cuda $0 release whillats"
        echo "  $0 --whillats-accel=metal release whillats   # macOS Metal"
        echo "  $0 debug whillats           # Host debug with whillats"
        echo "  $0 release whillats         # Host release with whillats"
        echo "  $0 ios whillats             # iOS debug with whillats"
        echo "  $0 ios debug whillats       # iOS debug with whillats"
        echo "  $0 ios release whillats     # iOS release with whillats"
        echo ""
        echo "Notes:"
        echo "  • Always runs from the script’s repo root (dirname of build.sh), not your current directory."
        echo "  • Writes .gclient, runs gclient sync when src/ is missing or incomplete; checkout + build use src/."
        echo "  • Removes untracked gclient “spill” dirs at repo root (api/, third_party/, out/, …) if git does not"
        echo "    track files under them — avoids duplicate trees next to src/. Tracked dirs are never deleted."
        echo "  • BUILD_RM_ROOT_CIPD=1 deletes root .cipd (large; re-fetched on next sync)."
        echo "  • BUILD_KEEP_SRC_OUT=1 keeps src/out across runs (default: remove src/out after sync for disk space)."
        echo "  • Prefers Python 3.12 or 3.11 for depot_tools; warns on newer Python."
        echo "  • Manual ninja: export PATH=\"\$HOME/depot_tools:\$PATH\" so vpython3 is found."
        echo "  • Whillats CMake uses a clean build/ each time (no cmake --fresh; works on CMake < 3.24)."
        echo "  • Host binary: src/out/<debug|release>/directcall — on success, runs directcall --help (not on iOS)."
        echo "  • If cert.pem/key.pem are missing under src/ after build, self-signed certs are generated there."
        echo "  • last_build_commit.txt at repo root records the outer-repo commit used for the last completed build."
        exit 1
    fi
fi
if [ $# -ge 2 ]; then
    if [[ "$2" == "whillats" ]]; then
        ENABLE_WHILLATS="true"
    elif [[ "$2" == "debug" || "$2" == "release" ]] && [[ "$IOS_BUILD" == "true" ]]; then
        # Second parameter can override build type for iOS builds
        BUILD_TYPE="$2"
    fi
fi
if [ $# -ge 3 ]; then
    if [[ "$3" == "whillats" ]] && [[ "$IOS_BUILD" == "true" ]]; then
        ENABLE_WHILLATS="true"
    fi
fi

# Handle whillats building
if [[ "$ENABLE_WHILLATS" == "true" ]]; then
    # Clean up previous iOS build artifacts before building whillats
    if [ "$IOS_BUILD" = "true" ]; then
        echo "Cleaning previous iOS build directories..."
        rm -rf "$WHILLATS_DIR/build-ios" "$SRC_DIR/out/ios_arm64"
    fi
    echo "Whillats option enabled: building whillats..."
    if [ "$IOS_BUILD" = "true" ]; then
        echo "Building whillats for iOS ($BUILD_TYPE)..."
        if [ ! -d "$WHILLATS_DIR" ] || [ -z "$(ls -A "$WHILLATS_DIR" 2>/dev/null)" ]; then
            echo "Initializing parent submodule: modules/third_party/whillats"
            pushd .
            echo "pwd: $PWD"
            cd $SRC_DIR/modules/third_party/whillats
            git fetch origin "$SRC_GIT_REF"
            git checkout "$SRC_GIT_REF"
            popd
            git submodule update --init --recursive modules/third_party/whillats
        else
            echo "Submodule exists; skipping update to preserve local edits."
        fi
        
        # Build iOS whillats with the specified build type
        if [ "$BUILD_TYPE" = "debug" ]; then
            (cd "$WHILLATS_DIR" && make ios-debug)
            WHILLATS_BUILD_DIR="Debug"
        else
            (cd "$WHILLATS_DIR" && make ios)
            WHILLATS_BUILD_DIR="Release"
        fi
        
        # Verify framework was built (check both lib and normalized bin directories)
        if [ -d "$WHILLATS_DIR/build-ios/lib/$WHILLATS_BUILD_DIR/whillats.framework" ] || [ -d "$WHILLATS_DIR/build-ios/bin/${BUILD_TYPE}/whillats.framework" ]; then
            echo "✅ whillats.framework ($BUILD_TYPE) built successfully"
            # Show where the framework was found
            if [ -d "$WHILLATS_DIR/build-ios/lib/$WHILLATS_BUILD_DIR/whillats.framework" ]; then
                ls -la "$WHILLATS_DIR/build-ios/lib/$WHILLATS_BUILD_DIR/whillats.framework/"
            fi
            if [ -d "$WHILLATS_DIR/build-ios/bin/${BUILD_TYPE}/whillats.framework" ]; then
                ls -la "$WHILLATS_DIR/build-ios/bin/${BUILD_TYPE}/whillats.framework/"
            fi
        else
            echo "❌ whillats.framework ($BUILD_TYPE) build failed"
            echo "Checked locations:"
            echo "  - $WHILLATS_DIR/build-ios/lib/$WHILLATS_BUILD_DIR/whillats.framework"
            echo "  - $WHILLATS_DIR/build-ios/bin/${BUILD_TYPE}/whillats.framework"
            exit 1
        fi
    else
        echo "Building whillats for host platform ($BUILD_TYPE)..."
        if [ ! -d "$WHILLATS_DIR" ] || [ -z "$(ls -A "$WHILLATS_DIR" 2>/dev/null)" ]; then
            echo "Initializing parent submodule: modules/third_party/whillats"
            git submodule update --init --recursive modules/third_party/whillats
        fi
        build_whillats "$BUILD_TYPE"
    fi
fi

# Set binary path and build dir based on build type and SRC_DIR
if [ "$IOS_BUILD" = "true" ]; then
    BINARY_PATH="$SRC_DIR/out/ios_arm64/directcall"
    BUILD_DIR="$SRC_DIR/out/ios_arm64"
elif [ "$BUILD_TYPE" = "debug" ]; then
    BINARY_PATH="$SRC_DIR/out/debug/directcall"
    BUILD_DIR="$SRC_DIR/out/debug"
else
    BINARY_PATH="$SRC_DIR/out/release/directcall"
    BUILD_DIR="$SRC_DIR/out/release"
fi

# Clean build output directory for ARM64 to avoid stale references to Clang 20
if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    echo "Cleaning build output directory: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

# Check if binary already exists and if code has updates
LAST_BUILD_COMMIT=""
if [ -f "../$LAST_BUILD_COMMIT_FILE" ]; then
    LAST_BUILD_COMMIT=$(cat ../$LAST_BUILD_COMMIT_FILE)
fi

# Check if binary exists and is executable
if [ -f "$BINARY_PATH" ] && [ -x "$BINARY_PATH" ]; then
    # Test if binary can run (e.g., by checking version or help output)
    if "$BINARY_PATH" --help >/dev/null 2>&1; then
        echo "Binary at $BINARY_PATH is runnable. Removing any WebRTC spill at repo root..."
        cd "$REPO_ROOT"
        remove_webrtc_spill_at_root
        cd src
    else
        echo "Binary at $BINARY_PATH exists but is not runnable. Skipping cleanup of other directories."
    fi
else
    echo "Binary at $BINARY_PATH does not exist or is not executable. Skipping cleanup of other directories."
fi

if [ "$IOS_BUILD" = "true" ]; then
    echo "Building WebRTC project for iOS (whillats: $ENABLE_WHILLATS)..."
    # iOS-specific build configuration
    if [ "$BUILD_TYPE" = "debug" ]; then
        IOS_DEBUG_FLAG="true"
    else
        IOS_DEBUG_FLAG="false"
    fi
    IOS_ARGS="target_environment=\"device\" target_os=\"ios\" target_cpu=\"arm64\" ios_deployment_target=\"16.4.0\" is_debug=$IOS_DEBUG_FLAG rtc_include_opus=true rtc_build_examples=true rtc_enable_symbol_export=true mac_deployment_target=\"15.0\" mac_min_system_version=\"15.0\""
    if [ "$ENABLE_WHILLATS" = "true" ]; then
        IOS_ARGS="$IOS_ARGS rtc_use_speech_audio_devices=true"
    else
        IOS_ARGS="$IOS_ARGS rtc_use_speech_audio_devices=false"
    fi
    
    # Clean iOS build directory
    echo "Cleaning iOS build directory..."
    (cd $SRC_DIR && rm -rf out/ios_arm64)
    
    echo "Generating iOS build configuration..."
    (cd $SRC_DIR && gn gen out/ios_arm64 --args="$IOS_ARGS")
    
    # Ensure whillats framework is available before WebRTC build if enabled
    if [ "$ENABLE_WHILLATS" = "true" ]; then
        # Use the normalized framework location (the Makefile copies it to bin/debug or bin/release)
        WHILLATS_FRAMEWORK_SRC="$WHILLATS_DIR/build-ios/bin/${BUILD_TYPE}/whillats.framework"
        WHILLATS_FRAMEWORK_DST="$WHILLATS_DIR/build-ios/bin/debug/whillats.framework"
        
        # Create symlink only if we're building release mode (debug already has the framework in the right place)
        if [ "$BUILD_TYPE" = "release" ]; then
            mkdir -p "$(dirname "$WHILLATS_FRAMEWORK_DST")"
            [ -d "$WHILLATS_FRAMEWORK_SRC" ] && ln -sfn "$WHILLATS_FRAMEWORK_SRC" "$WHILLATS_FRAMEWORK_DST" && echo "Pre-linked whillats.framework ($BUILD_TYPE) for WebRTC build" || echo "Warning: whillats.framework not found at $WHILLATS_FRAMEWORK_SRC"
        else
            echo "Using whillats.framework directly from bin/debug (no symlink needed)"
        fi
    fi
    
    echo "Building WebRTC framework and directcall for iOS (single job to avoid toolchain races)..."
    # Build both the WebRTC framework and directcall binary
    (cd $SRC_DIR && ninja -C out/ios_arm64 -j1 sdk:framework_objc directcall)
    echo "iOS build completed."
    
    # Verify both framework and binary were built
    if [ -d "$SRC_DIR/out/ios_arm64/WebRTC.framework" ]; then
        echo "✅ WebRTC.framework built successfully"
        ls -la "$SRC_DIR/out/ios_arm64/WebRTC.framework/"
    else
        echo "❌ WebRTC.framework build failed"
        exit 1
    fi
    
    # Update binary path for iOS
    BINARY_PATH="$SRC_DIR/out/ios_arm64/directcall"
    
elif [ "$BUILD_TYPE" = "debug" ]; then
    echo "Building WebRTC project (debug, whillats: $ENABLE_WHILLATS)..."
    (cd $SRC_DIR && gn gen out/debug --args="is_debug=true rtc_include_opus=true rtc_enable_symbol_export=true rtc_build_examples=true rtc_use_speech_audio_devices=$ENABLE_WHILLATS $EXTRA_ARGS")
    (cd $SRC_DIR && ninja -C out/$BUILD_TYPE directcall)
    echo "Debug build completed."
else
    echo "Building WebRTC project (release, whillats: $ENABLE_WHILLATS)..."
    (cd $SRC_DIR && gn gen out/release --args="is_debug=false symbol_level=0 rtc_include_opus=true rtc_enable_symbol_export=true rtc_build_examples=true rtc_use_speech_audio_devices=$ENABLE_WHILLATS $EXTRA_ARGS")
    (cd $SRC_DIR && ninja -C out/$BUILD_TYPE directcall)
    echo "Release build completed."
fi


# Store the current commit hash as the last built commit
echo "$CURRENT_COMMIT" > ../$LAST_BUILD_COMMIT_FILE

if [ -f "cert.pem" ] && [ -f "key.pem" ]; then
    echo "Certificates already exist, skipping creation."
else
    # Creating certificates
    openssl req -x509 -newkey rsa:4096 -keyout key.pem -out cert.pem -sha256 -days 3650 -nodes -subj "/C=US/ST=CA/L=SanFrancisco/O=Acme/OU=Development/CN=WebRTCsays.ai"    
fi

if [ "$IOS_BUILD" = "true" ]; then
    echo "iOS build completed successfully!"
    echo "iOS binary location: $BINARY_PATH"
    echo "WebRTC framework location: $SRC_DIR/out/ios_arm64/WebRTC.framework"
    echo "Note: iOS binaries cannot be executed directly on macOS."
    echo "To deploy to iOS device or simulator, use Xcode or appropriate iOS deployment tools."
    if [ -f "$BINARY_PATH" ]; then
        echo "✅ iOS binary exists and is ready for deployment"
        ls -la "$BINARY_PATH"
    else
        echo "❌ iOS binary not found at expected location"
    fi
    
    if [ -d "$SRC_DIR/out/ios_arm64/WebRTC.framework" ]; then
        echo "✅ WebRTC.framework ready for iOS app integration"
        echo "Framework size: $(du -sh "$SRC_DIR/out/ios_arm64/WebRTC.framework" | cut -f1)"
    else
        echo "❌ WebRTC.framework not found at expected location"
    fi
else
    # Regular macOS/Linux binary execution
    LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$REPO_ROOT/src/modules/third_party/whillats/build/lib/$BUILD_TYPE"
    if [ "$ENABLE_WHILLATS" = "true" ]; then
        LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$REPO_ROOT/src/modules/third_party/whillats/build/lib/$BUILD_TYPE"
    fi

    echo "Running binary as LD_LIBRARY_PATH=$LD_LIBRARY_PATH $BINARY_PATH to show help..."
    LD_LIBRARY_PATH=$LD_LIBRARY_PATH $BINARY_PATH --help
fi

#echo "Running binary in callee mode to test signaling..."    
#$BINARY_PATH --mode=callee  --webrtc_cert_path=cert.pem  --webrtc_key_path=key.pem --user_name=Slim 3.93.50.189:3456
