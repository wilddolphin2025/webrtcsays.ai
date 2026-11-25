# WebRTCsays.ai Build Instructions

## Getting the Code
To clone the repository and build the project, run:
```bash
git clone https://github.com/wilddolphin2025/webrtcsays.ai webrtcsays.ai
cd webrtcsays.ai
git checkout talkingface   # primary development branch (also: develop, main, …)
# Build (choose one of the following):
./build.sh release
./build.sh debug
./build.sh release whillats
./build.sh debug whillats
```

## Overview
WebRTCsays.ai is a project that brings AI-powered speech capabilities to WebRTC. This guide explains how to build the project using the provided `build.sh` script, including all available build options and platform notes.

---

## Prerequisites
- **Linux (x86_64, arm64/aarch64) or macOS**
- **Python 3**
- **depot_tools** ([installation guide](https://commondatastorage.googleapis.com/chrome-infra-docs/flat/depot_tools/html/depot_tools_tutorial.html))
- **C++ build tools** (e.g., clang, ninja, cmake, make, etc.)
- **OpenSSL** (for certificate generation)
- **CUDA Toolkit 12.8+ (Optional)** - For GPU acceleration of Whisper/LLaMA models. See [CUDA.md](CUDA.md) for compatibility details.

### Install depot_tools
```bash
# Clone depot_tools and add to PATH
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git ~/depot_tools
export PATH=~/depot_tools:$PATH
```

---

## Building WebRTCsays.ai

All builds are managed by the `build.sh` script in the project root. The script will:
- Set up the source tree and dependencies
- Configure the build for your platform/architecture
- Build the `directcall` binary
- Optionally enable AI speech features
- Generate self-signed certificates if needed

### Usage
```text
./build.sh [-b REF | --branch REF | --branch=REF] [--whillats-accel MODE] [debug|release|ios] [whillats]
```

| Argument / env | Purpose |
|----------------|---------|
| `debug` / `release` | Debug symbols vs optimized build (default if omitted: `debug` for host). |
| `ios` | iOS build; optional second arg `debug` or `release`. |
| `whillats` | Enable Whisper + LLaMA + TTS (whillats) in-tree; **host whillats GGML backend is CPU by default** (see below). |
| `-b REF`, `--branch REF`, `--branch=REF` | Git ref for the **`src/`** checkout only; outer clone branch unchanged. |
| `SRC_BRANCH=REF` | Same as `-b` if no flag on the command line. |
| `--whillats-accel MODE` | `cpu` (default) · `cuda` · `metal` (macOS) · `auto` (legacy: Metal on mac, else CUDA if `nvcc`). |
| `WHILLATS_ACCEL` | Same as `--whillats-accel` when unset on the CLI. |
| `WHILLATS_CUDA_ARCH` | Optional for `cuda` / `auto` CUDA path, e.g. `80-real` (avoids relying on `nvidia-smi`). |
| `BUILD_RM_ROOT_CIPD=1` | Delete repo-root `.cipd` before sync (large; re-downloaded next sync). |

Run `./build.sh` with an invalid first token (e.g. `./build.sh help`) to print the full in-script usage and **Notes**.

#### Examples
- **Debug build (default):**
  ```bash
  ./build.sh
  # or
  ./build.sh debug
  ```
- **Release build:**
  ```bash
  ./build.sh release
  ```
- **Debug build with whillats (CPU GGML, default):**
  ```bash
  ./build.sh debug whillats
  ```
- **Release with whillats on CUDA:**
  ```bash
  WHILLATS_ACCEL=cuda ./build.sh release whillats
  # or
  ./build.sh --whillats-accel=cuda release whillats
  ```
- **macOS Metal for whillats:**
  ```bash
  ./build.sh --whillats-accel=metal release whillats
  # or legacy auto-detect:
  WHILLATS_ACCEL=auto ./build.sh release whillats
  ```
- **Explicit `src/` ref (e.g. CI or pinned branch):**
  ```bash
  ./build.sh -b talkingface release whillats
  ```
- **Release iOS build with whillats:**
  ```bash
  ./build.sh ios release whillats
  ```
- **Debug iOS build without whillats:**
  ```bash
  ./build.sh ios debug
  ```

### What do the AI options do?
- **`whillats`**: Enables Whisper, LLaMA, and TTS integration built in-tree. The **WebRTC** side links against whillats headers/libs; **GGML** acceleration for whillats itself is **CPU unless** you set `WHILLATS_ACCEL` / `--whillats-accel` to `cuda`, `metal`, or `auto`.
- If `whillats` is omitted, the build uses standard audio devices without that AI stack.

### build.sh behavior (notes)
- **Working directory:** The script **`cd`s to the directory that contains `build.sh`** (repo root), not your current shell directory.
- **Layout:** Writes `.gclient` and runs **`gclient sync`** when `src/` is missing or incomplete; checkout and **GN/ninja** build use **`src/`**.
- **Root cleanup:** Removes **untracked** gclient “spill” directories at the repo root (`api/`, `third_party/`, `out/`, …) when **git does not track** files under them—so you do not keep a duplicate WebRTC tree next to `src/`. **Tracked** trees are never deleted.
- **Python:** Prefers **3.12** or **3.11** for depot_tools; warns on newer Python.
- **Whillats CMake:** Uses a clean `build/` directory each time; does **not** use `cmake --fresh` (works on **CMake before 3.24**, e.g. Ubuntu 22.04).
- **After a successful host build:** Runs **`directcall --help`** (skipped for iOS).
- **`last_build_commit.txt`:** Written at the **repo root** after a completed build, recording the **outer** repo commit that was built.

---

## Platform/Architecture Notes
- The script auto-detects your architecture (`x86_64`, `arm64`, etc.) and sets up sysroots/toolchains as needed.
- On ARM64 (aarch64), the script uses the system Clang and may build additional runtime libraries if missing.
- On x86_64, standard Chromium sysroots and toolchains are used.

### GPU acceleration (whillats / GGML)
- **Default:** **`WHILLATS_ACCEL=cpu`** — whillats builds **CPU-only** (no implicit CUDA even if `nvcc` is installed). Good for CI, laptops, and stable smoke builds.
- **CUDA:** Set **`WHILLATS_ACCEL=cuda`** (and ensure **`nvcc`** is on `PATH`). Optionally set **`WHILLATS_CUDA_ARCH`** (e.g. `80-real`) for a fixed architecture.
- **Metal (macOS):** **`WHILLATS_ACCEL=metal`**.
- **Legacy behavior:** **`WHILLATS_ACCEL=auto`** — Metal on macOS, else CUDA if `nvcc` exists, else CPU.
- **System / toolchain issues:** See [CUDA.md](CUDA.md) for CUDA versions, glibc, and troubleshooting.

---

## Output
- The main binary is built as:
  - `src/out/debug/directcall` (for debug)
  - `src/out/release/directcall` (for release)
- Self-signed certificates (`cert.pem`, `key.pem`) are generated in `src/` if not present.

---

## Running the Application
After building, the script will attempt to run the binary automatically. You can also run it manually:
```bash
# Show help
src/out/debug/directcall --help

# Example: Run as callee
src/out/debug/directcall --mode=callee 127.0.0.1:3456 --encryption --webrtc_cert_path=src/cert.pem --webrtc_key_path=src/key.pem

# Example: Run as caller
src/out/debug/directcall --mode=caller 127.0.0.1:3456 --encryption --webrtc_cert_path=src/cert.pem --webrtc_key_path=src/key.pem

# With Whisper enabled
src/out/debug/directcall --mode=callee 127.0.0.1:3456 --whisper --encryption
src/out/debug/directcall --mode=caller 127.0.0.1:3456 --whisper --encryption
```

---

## Using config.json
The `config.json` file allows you to predefine all runtime options for WebRTCsays.ai, such as mode, address, encryption, model paths, and more. This is useful for repeatable setups or automation.

To use a configuration file, pass it to the application with the `--config` flag:

```bash
src/out/debug/directcall --config config.json
```

You can still override individual options on the command line if needed. The application will load settings from `config.json` and use them as defaults unless overridden.

**Note:** The self-signed certificates (`cert.pem` and `key.pem`) are automatically created in the `src/` directory if they do not exist. Make sure your config paths point to `src/cert.pem` and `src/key.pem`.

---

## Example config.json
Below is an example `config.json` for running WebRTCsays.ai. Adjust paths and options as needed for your environment. (No passwords or sensitive data are included.)

```json
{
    "mode": "callee",
    "address": "3.93.50.189:3456",
    "user_name": "Slim",
    "room_name": "room101",
    "encryption": true,
    "whisper": true,
    "llama": false,
    "video": true,
    "bonjour": false,
    "language": "en",
    "whisper_model": "${HOME}/Public/models/ggml-small.bin",
    "llama_model": "${HOME}/Public/models/llava-llama-3-8b-v1_1-f16.gguf",
    "llava_mmproj": "${HOME}/Public/models/llava-llama-3-8b-v1_1-mmproj-f16.gguf",
    "webrtc_cert_path": "src/cert.pem",
    "webrtc_key_path": "src/key.pem",
    "webrtc_speech_initial_playout_wav": "",
    "turns": [
        ["turn:3.93.50.189:5349?transport=udp", "<username>", "<password>"]
    ],
    "camera": "0,640x480@30",
    "whispers": {
        "en": ["Hello, how are you?", "Please describe the image."],
        "zh": ["你好，你好吗？", "请描述图片。"]
    }
}
```

---

## config.json Field Descriptions
Below is an explanation of each field in the example config.json:

- **mode**: Operation mode for the app. Usually "callee" or "caller".
- **address**: The IP address and port to connect to or listen on (e.g., "3.93.50.189:3456").
- **user_name**: Your display/user name in the session.
- **room_name**: (Optional) Room identifier for multi-user or group sessions.
- **encryption**: Set to `true` to enable end-to-end encryption using the provided cert/key.
- **whisper**: Set to `true` to enable Whisper AI speech-to-text integration.
- **llama**: Set to `true` to enable LLaMA language model features (if supported).
- **video**: Set to `true` to enable video streaming; `false` for audio-only.
- **bonjour**: Set to `true` to enable Bonjour/zeroconf service discovery.
- **language**: Language code for speech recognition (e.g., "en", "zh").
- **whisper_model**: Path to the Whisper model file (e.g., "${HOME}/Public/models/ggml-small.bin").
- **llama_model**: Path to the LLaMA model file (if used).
- **llava_mmproj**: Path to the LLaVA multimodal projection model (if used).
- **webrtc_cert_path**: Path to the TLS certificate file (should be "src/cert.pem").
- **webrtc_key_path**: Path to the TLS private key file (should be "src/key.pem").
- **webrtc_speech_initial_playout_wav**: (Optional) Path to a WAV file to play at session start.
- **turns**: List of TURN server entries for NAT traversal. Each entry is a list: [URL, username, credential].
- **camera**: Camera device and settings (e.g., "0,640x480@30" for device 0 at 640x480, 30fps. NOTE: This works on Linux).
- **whispers**: Predefined prompt phrases for Whisper, organized by language code.

Adjust these fields as needed for your deployment and use case.

---

## CetaCall on Apple Silicon

This is a quality-test path, not the default call. Opus stays the default. CetaCall is compiled only when you pass `cetacall` to `build.sh` on macOS. Linux and the arm64 Frame build do not include it.

The Core ML packages are fetched from `wilddolphin2025/liquidcodec-ane`. The default ref is `cursor/liquidcodec-stream-51d0` (draft PR 1) until that branch merges. Override with `CETACALL_REPO` and `CETACALL_REF`. This branch does not convert either model. Weights stay out of git. They are Liquid AI's Liquid-Codec under the LFM Open License v1.0 (commercial use only below USD 10M annual revenue). See `scripts/cetacall/NOTICE`.

`models/cetacall.json` from that branch is the manifest (upstream name; `fetch_models.sh` renames it to `cetacall.json` and leaves a `cetacall.json` symlink). The decoder half is a stateless window: `ctx_frames` 20 and `new_frames` 1, 2, or 4, inputs `lat` and `valid`, output `hidden`, plus `lc_tail_f32.bin`. The encoder half is a one-frame step (`encoder_kind` `stateful_io`). Each call takes 2400 samples, the 80 ms frame plus 480 samples of real lookahead, and five state tensors (`hist`, `k_cache`, `v_cache`, `conv_state`, `pos`) that come back updated. The model applies the first-call left pad itself when those tensors are zero. The glue waits for the 480 real samples and does not reflect-pad them. A manifest without `encoder_kind` still uses the older stateless window (`ctx+new`, or `new` with no lookahead). `--cetacall-ctx` and `--cetacall-new` override the manifest and must match the decoder package.

Calculated latency, not a measurement. For the streaming encoder, the first packet waits 80 ms plus the 20 ms lookahead, the receiver then waits `(new_frames - 1) * 80` ms, and the CPU STFT tail holds 20 ms. At `new_frames=1` that is **120 ms**. At `new_frames=4` it is 360 ms. The older stateless 20-frame window, with no lookahead, is still 3140 ms. Left context is lookback. NetEq's jitter buffer and the Core ML runtime are not in these numbers. `decoder_stats.json` records `prediction_ms_mean` only after a call on a Mac. Payload is 28 bytes per 80 ms frame (2800 bps on the wire). The information rate is about 1646 bps. SDP name is `CETACALL` at a 48 kHz clock, because NetEq does not accept 24 kHz; the codec itself is 24 kHz mono.

From a checkout such as `~/Public/webrtcsays-m156` or `~/Dev/webrtcsays.ai`:

```bash
cd ~/Public/webrtcsays-m156   # or ~/Dev/webrtcsays.ai
git fetch origin liquidcodec-ane
git checkout liquidcodec-ane

# One command from the gclient parent. It syncs, runs the pinned depot_tools gn,
# builds directcall, fetches models (skipped when they are already compiled),
# runs xcrun coremlcompiler on every .mlpackage, and rewrites cetacall.json
# to those .mlmodelc paths. It then prints the callee and caller lines.
./build.sh release cetacall
# ./build.sh -b liquidcodec-ane release cetacall

# Same call the build prints. Both sides inject the tone. It runs N seconds,
# then fails the shell if a log shows Check failed, a bad code, or a session
# restart, or if either direction sent fewer than 90% of duration/80ms packets.
# It does not use GNU timeout. Default directcall AEC is on; this script passes
# --aec=off so the injected clip is not altered. Use --aec=on for a mic loop.
src/scripts/cetacall/run_local_call.sh

# iOS frameworks for the RobotPhone app (a separate repo). Unsigned.
# Writes <gclient>/out/ios-arm64/WebRTC.framework, direct.framework, and models/.
# ./build.sh -b liquidcodec-ane ios release cetacall

# Quality ladder. One command prepares clips, models, and the binary, then
# writes scripts/cetacall/out/quality/<timestamp>/quality.csv and quality.md.
# --stages 1 is the offline codec. --stages 2 is packing and runs on Linux.
# --quick is the short matrix. Stage 5 injects loss and jitter in-process.
src/scripts/cetacall/quality_ladder.sh --quick
# src/scripts/cetacall/quality_ladder.sh --stages 1-6

# A hand-supplied clip still works. 24 kHz mono is the codec rate.
# afconvert -f WAVE -d LEI16@24000 input.flac "$HOME/cetacall-clip.wav"
# src/scripts/cetacall/run_local_call.sh --inject-wav "$HOME/cetacall-clip.wav" --duration 20

# A live mic uses AEC3, NS, and AGC unless you pass --aec=off. The script above
# passes --aec=off. Headphones, so the mic does not hear the speaker.
BIN=src/out/release/directcall
MODELS=scripts/cetacall/models
mkdir -p "$HOME/cetacall-record/callee" "$HOME/cetacall-record/caller"

"$BIN" --mode=callee --user_name=alice --room_name=room \
  --no-encryption --no-websocket_signaling --no-video --no-whisper --no-llama \
  --audio-codec=cetacall \
  --cetacall-decode=ane \
  --cetacall-model-dir="$MODELS" \
  --record-dir="$HOME/cetacall-record/callee" \
  127.0.0.1:3456

"$BIN" --mode=caller --user_name=bob --target_name=alice --room_name=room \
  --no-encryption --no-websocket_signaling --no-video --no-whisper --no-llama \
  --audio-codec=cetacall \
  --cetacall-decode=ane \
  --cetacall-model-dir="$MODELS" \
  --inject-wav="$HOME/cetacall-clip.wav" \
  --record-dir="$HOME/cetacall-record/caller" \
  --duration=20 \
  127.0.0.1:3456
```

Two Macs on a LAN: run the callee the same way with that Mac's LAN address instead of `127.0.0.1`, and point the caller at the same `192.168.x.x:3456`. Both machines need the fetched models and a `cetacall` build.

Listen and compare:

- `~/cetacall-record/caller/codec_in_24k.wav` is the 24 kHz audio CetaCall encoded.
- `~/cetacall-record/callee/codec_out_24k.wav` is the 24 kHz audio after the Core ML decoder and CPU tail, without the priming silence.
- `caller/input_48k.wav` and `callee/output_48k.wav` are what WebRTC sent and played, including the wait before the first chunk.
- `caller/encoder_stats.json` and `callee/decoder_stats.json` have payload bitrate, packet size, chunk size, the calculated latency, and `prediction_ms_mean` for the Core ML call.
- `tap_stats.json` is the same payload accounting for an Opus run.

Repeat the caller and callee with `--cetacall-decode=cpu` to compare that prediction time with the ANE run. Nothing in this repo has measured either one.

Opus A/B, same clip, same record dirs (use a different folder so the WAVs are not overwritten):

```bash
# Comparable: Opus floor is 6000 bps. CetaCall's information rate is about 1646 bps.
"$BIN" --mode=callee --user_name=alice --room_name=room \
  --no-encryption --no-websocket_signaling --no-video \
  --audio-codec=opus --opus-bitrate=6000 \
  --record-dir="$HOME/cetacall-record/opus6k-callee" \
  127.0.0.1:3456

"$BIN" --mode=caller --user_name=bob --target_name=alice --room_name=room \
  --no-encryption --no-websocket_signaling --no-video \
  --audio-codec=opus --opus-bitrate=6000 \
  --inject-wav="$HOME/cetacall-clip.wav" \
  --record-dir="$HOME/cetacall-record/opus6k-caller" \
  --duration=20 \
  127.0.0.1:3456

# Normal speech rate.
# same commands with --opus-bitrate=32000 and record dirs opus32k-callee / opus32k-caller
```

`--audio-codec` omitted is the existing Opus path with no extra flags. The manifest contract is `scripts/cetacall/cetacall.example.json`.

### Latency: low-latency jitter buffer

For `--audio-codec=cetacall` the receive jitter buffer (NetEq) runs in a low-latency mode by default: `--jitter-max-ms=80` caps NetEq's target delay, and the same cap (plus NetEq's 20 ms window) bounds the playout delay NetEq accelerates down to, with fast accelerate on. Below the cap NetEq still adapts to measured jitter. `--jitter-max-ms=160` is a balanced setting, `--jitter-max-ms=0` is stock NetEq with no cap (the old behavior), and `--no-jitter-fast-accelerate` turns fast accelerate off. The startup line `Jitter buffer: ...` shows what is in effect. Opus calls keep stock NetEq unless the flag is given.

The stats line reports `jitter_buffer_ms` for the last interval (it was the whole-call average, which lagged changes by tens of seconds), `jitter_buffer_avg_ms` for the call, and `jitter_buffer_target_ms`, NetEq's target. `mouth_to_ear_est_ms` uses the interval value. Once DTLS has picked the SRTP cipher, an `On-wire (negotiated): ...` line prints the per-packet bytes with the real tag (4 bytes for SHA1_32).

The factory quality stage uses the same cap. `CETACALL_QUALITY_STALL_MS=150` makes it hold packets for 150 ms every 5 s and deliver them in a burst, like a Wi-Fi stall:

```bash
CETACALL_QUALITY_STALL_MS=150 src/out/lowlat/directcall --quality=factory \
  --inject-wav=speech.wav --record-dir=/tmp/q --cetacall-model-dir=<models> --jitter-max-ms=80
```

Measured in that simulation (speech, 18 s, Core ML ANE): with no stalls the mean packet wait is 30 ms at any setting. With 150 ms stalls every 5 s, stock NetEq waits 154 ms on average (target 120 ms) and the 80 ms cap waits 90 ms (target 80 ms) for 0.5 percentage points more concealment. With 300 ms stalls: 289 ms stock, 158 ms capped, concealment 1.5% vs 3.9%.

Build the Mac binary into its own directory, so a callee running from `out/release` or another directory is not relinked under it:

```bash
src/scripts/cetacall/build_mac_directcall.sh lowlat   # -> src/out/lowlat/directcall
```

### Echo cancellation on macOS

`--aec=on` (the default) now runs the Mac's audio through Apple's Voice Processing I/O unit, the canceller FaceTime uses, instead of the CoreAudio HAL device plus WebRTC's software AEC3. VPIO cancels against exactly what the speaker played, so NetEq's accelerate and expand don't matter to it. `--aec=aec3` keeps the old HAL + AEC3 path; `--aec=off` turns cancellation off. iOS always uses VPIO. The startup log prints `Audio device:` and `Echo canceller:`; every stats second prints

    echo aec=vpio send_dbfs=-52.1 recv_dbfs=-24.0 erl_db=na erle_db=na aec_delay_ms=-1 divergent=-1.000

`send_dbfs` is what we transmit after cancellation and `recv_dbfs` what we play. While only the far end talks, `send_dbfs` should stay near the room's noise floor (about -50 dBFS or lower); a send level that rises and falls with `recv_dbfs` is echo leaking. With `--aec=aec3` the line also has AEC3's ERL, ERLE, delay, and divergent-filter fraction. Decoding for playout runs on its own thread with a 20 ms ring ahead of the VPIO callback (`CETACALL_VPIO_PREBUFFER_MS`), like the HAL device's ring.

Two devices in the same room form an acoustic loop no canceller can see: the Mac's speaker reaches the iPhone's mic, and the iPhone's speaker reaches the Mac's mic. Each canceller only removes its own speaker. For a same-room test, use headphones on one side or the iPhone's earpiece.

`directcall --quality=aec --inject-wav=<render.wav> --record-dir=<dir>` is an offline check of AEC3: the WAV plays through a simulated speaker-to-mic path (40 ms delay, 40 ms room tail, -6 dB; `AEC_TEST_DELAY_MS`, `AEC_TEST_GAIN_DB`, `AEC_TEST_JUMP_MS`) into APM, and it prints ERLE per second. Feeding it the factory stage's `quality_out.wav` with `CETACALL_QUALITY_STALL_MS=150` (80 ms cap, accelerate and expand active) gave the same steady ERLE as the unstretched speech, about 68 dB per second after convergence. A 30 ms jump in the device delay cost one to two seconds of leak (13.7 dB worst second). NetEq doesn't change that delay, because the canceller's reference is the audio after NetEq.

### Bandwidth: tight packing and Opus

Each code is in `[0, 2016)`, so it fits in 11 bits. Twelve codes are 132 bits. `--cetacall-pack=tight` stores that in **17 bytes** and adds no payload header: the RTP sequence number and timestamp already identify the packet, and SDP `pack=tight` with `fpp` fixes how many frames are inside. The length is unique too (17, 33, or 66 bytes). `--cetacall-pack=u16` keeps the current header plus 12 little-endian codes: 28, 52, or 100 bytes for 1, 2, or 4 frames. `--cetacall-frames-per-packet` is 1, 2, or 4. A packet of several frames is parsed into 80 ms frames so NetEq's 120 ms limit still holds.

Calculated on-wire rate, IPv4 20 + UDP 8 + RTP 12, no header extensions. The SRTP column adds a 16-byte (GCM) tag; CetaCall calls offer SHA1_32, a 4-byte tag, so a u16 one-frame call is 7.2 kbps on the wire. The quality calls use `--no-encryption`, so the plain column is the one getStats should approach.

| pack | frames/packet | payload | payload kbps | IPv4/UDP/RTP kbps | plus SRTP tag |
|---|---:|---:|---:|---:|---:|
| u16 | 1 | 28 | 2.800 | 6.800 | 8.400 |
| tight | 1 | 17 | 1.700 | 5.700 | 7.300 |
| tight | 2 | 33 | 1.650 | 3.650 | 4.450 |
| tight | 4 | 66 | 1.650 | 2.650 | 3.050 |

`scripts/cetacall/bench_bandwidth.sh --theoretical` prints the full matrix, including Opus at 6, 8, 12, 16, 24, and 32 kbps, ptime 20/40/60, and DTX on or off. Opus sizes in that table are the nominal target. DTX is smaller only while the signal is quiet, which a real call measures and the table does not invent.

On the Mac, after `./build.sh release cetacall`. The bench uses that binary and `scripts/cetacall/models`. With no `--samples` it generates a tone, so it does not need a separate download.

```bash
src/scripts/cetacall/bench_bandwidth.sh --theoretical
src/scripts/cetacall/bench_bandwidth.sh --quick

# Speech clips, when you want WER rather than the generated tone:
src/scripts/cetacall/bench_bandwidth.sh \
  --samples "$HOME/cetacall-ane/samples" \
  --conversation "$HOME/speech/conversation.wav"

# These print sudo commands and do not run them.
scripts/cetacall/bench_bandwidth.sh --pcap --impair --theoretical
```

Each measured run writes `scripts/cetacall/out/bandwidth/<config>/caller.log` (getStats `send_bytes`, `send_packets`, `send_header_bytes`) and a WAV under `callee/`. The analyzer writes `bandwidth.csv` and, when a score exists, `bandwidth.svg` (quality vs on-wire kbps) and `bandwidth_wer.svg`. Mouth-to-ear in the CSV is an estimate: calculated codec latency + jitter buffer + half the RTT + mean Core ML prediction time. It is not an acoustic measurement.

Quality tools are optional and were not run here:

```bash
python3 -m pip install numpy soundfile pesq pystoi
# ViSQOL speech mode, if the package builds: python3 -m pip install visqol
# Whisper WER: python3 -m pip install openai-whisper
#   CETACALL_BENCH_WHISPER=1 python3 scripts/cetacall/bandwidth.py --analyze scripts/cetacall/out/bandwidth
# A .txt transcript with the same basename as the clip is the WER reference.
# Without one, Whisper transcribes the original and the decode and compares those texts.
```

ANE and CPU decode time per frame are `prediction_ms_mean` in `callee/decoder_stats.json`. The bench runs each CetaCall packing twice, `--cetacall-decode=ane` and `--cetacall-decode=cpu`. Nothing in this checkout has executed those calls.

## Troubleshooting
- If you encounter missing dependencies, ensure all prerequisites are installed.
- For Linux/ARM64, the script will attempt to build missing runtime libraries if needed.
- If the build fails, check the log and branch: use **`talkingface`** (or the branch you intend); run **`./build.sh help`** for usage.
- **`CMake Error: Unknown argument --fresh`:** Use a **`build.sh`** that includes the fix (no `--fresh`); upgrade CMake to 3.24+ if you use `--fresh` elsewhere.
- **CUDA / whillats:** Use **`WHILLATS_ACCEL=cuda`** only when you want GPU; default **CPU** avoids accidental heavy CUDA builds. See [CUDA.md](CUDA.md).
- **Disk space:** Full **`gclient sync`** + whillats + WebRTC needs many GB; free space or set **`BUILD_RM_ROOT_CIPD=1`** if you need to reclaim `.cipd`.

---

## Advanced
- **`last_build_commit.txt`** at the repo root records the outer-repo commit after a **completed** build (used to reason about incremental rebuilds).
- Manually clean outputs: delete **`src/out/`** (and whillats **`src/modules/third_party/whillats/build`** if rebuilding whillats).

---

## Contact
For issues or questions, please open an issue on the project's GitHub page.
