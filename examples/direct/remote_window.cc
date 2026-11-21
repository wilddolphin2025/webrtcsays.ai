/*
 *  (c) 2026, wilddolphin2025
 *  For WebRTCsays.ai project
 */

#include "remote_window.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "api/video/i420_buffer.h"
#include "remote_choice.h"

namespace {

constexpr int kMaxEdge = 1920;
constexpr uint32_t kSdlInitVideo = 0x00000020;
constexpr uint32_t kSdlWindowShown = 0x00000004;
constexpr int kSdlWindowPosCentered = 0x2FFF0000;
constexpr uint32_t kSdlPixelFormatAbgr8888 = 0x16762004u;

struct OpenVrFns {
  using InitFn = intptr_t (*)(int*, int);
  using ShutdownFn = void (*)();
  using InterfaceFn = intptr_t (*)(const char*, int*);
  using ErrorFn = const char* (*)(int);
  using CreateFn = int (*)(char*, char*, uint64_t*);
  using DestroyFn = int (*)(uint64_t);
  using WidthFn = int (*)(uint64_t, float);
  using TransformFn = int (*)(uint64_t, int, float*);
  using ShowFn = int (*)(uint64_t);
  using RawFn = int (*)(uint64_t, void*, uint32_t, uint32_t, uint32_t);

  void* lib = nullptr;
  InitFn init = nullptr;
  ShutdownFn shutdown = nullptr;
  InterfaceFn interface = nullptr;
  ErrorFn error_name = nullptr;
  void** table = nullptr;
  uint64_t overlay = 0;
  bool started = false;

  bool Load() {
    const char* paths[] = {
        "libopenvr_api.so",
        "/opt/steamvr/bin/linuxarm64/libopenvr_api.so",
        nullptr,
    };
    for (int i = 0; paths[i] && !lib; ++i) lib = dlopen(paths[i], RTLD_LAZY);
    if (!lib) return false;
    init = reinterpret_cast<InitFn>(dlsym(lib, "VR_InitInternal"));
    shutdown = reinterpret_cast<ShutdownFn>(dlsym(lib, "VR_ShutdownInternal"));
    interface = reinterpret_cast<InterfaceFn>(dlsym(lib, "VR_GetGenericInterface"));
    error_name = reinterpret_cast<ErrorFn>(dlsym(lib, "VR_GetVRInitErrorAsSymbol"));
    return init && shutdown && interface;
  }

  bool Start() {
    int error = 0;
    // VRApplication_Overlay. Does not replace the running scene app.
    init(&error, 2);
    if (error != 0) {
      const char* name = error_name ? error_name(error) : "unknown";
      std::fprintf(stderr, "openvr init failed: %s\n", name ? name : "unknown");
      return false;
    }
    error = 0;
    table = reinterpret_cast<void**>(interface("FnTable:IVROverlay_028", &error));
    if (error != 0 || !table) {
      std::fprintf(stderr, "openvr overlay IVROverlay_028 is not available\n");
      shutdown();
      return false;
    }
    char key[] = "webrtcsays.remote";
    char title[] = "WebRTCSays remote";
    const int created = reinterpret_cast<CreateFn>(table[1])(key, title, &overlay);
    if (created != 0) {
      std::fprintf(stderr, "openvr CreateOverlay failed: %d\n", created);
      shutdown();
      return false;
    }
    reinterpret_cast<WidthFn>(table[22])(overlay, 1.6f);
    float matrix[3][4] = {
        {1.f, 0.f, 0.f, 0.f},
        {0.f, 1.f, 0.f, 1.3f},
        {0.f, 0.f, 1.f, -2.0f},
    };
    // TrackingUniverseStanding. -Z is in front of the seated origin.
    reinterpret_cast<TransformFn>(table[33])(overlay, 1, &matrix[0][0]);
    const int shown = reinterpret_cast<ShowFn>(table[43])(overlay);
    if (shown != 0) {
      std::fprintf(stderr, "openvr ShowOverlay failed: %d\n", shown);
      reinterpret_cast<DestroyFn>(table[3])(overlay);
      shutdown();
      return false;
    }
    started = true;
    std::fprintf(stderr, "remote window: openvr overlay IVROverlay_028\n");
    return true;
  }

  bool Present(const uint8_t* rgba, int width, int height) {
    if (!started) return false;
    const int result = reinterpret_cast<RawFn>(table[62])(
        overlay, const_cast<uint8_t*>(rgba), static_cast<uint32_t>(width),
        static_cast<uint32_t>(height), 4);
    return result == 0;
  }

  void Stop() {
    if (started && table) {
      reinterpret_cast<DestroyFn>(table[3])(overlay);
      shutdown();
    }
    started = false;
    if (lib) {
      dlclose(lib);
      lib = nullptr;
    }
  }
};

struct SdlFns {
  void* lib = nullptr;
  int (*init)(uint32_t) = nullptr;
  void* (*window)(const char*, int, int, int, int, uint32_t) = nullptr;
  void* (*renderer)(void*, int, uint32_t) = nullptr;
  void* (*texture)(void*, uint32_t, int, int, int) = nullptr;
  int (*update)(void*, const void*, const void*, int) = nullptr;
  int (*clear)(void*) = nullptr;
  int (*copy)(void*, void*, const void*, const void*) = nullptr;
  void (*present)(void*) = nullptr;
  int (*poll)(void*) = nullptr;
  void (*destroy_texture)(void*) = nullptr;
  void (*destroy_renderer)(void*) = nullptr;
  void (*destroy_window)(void*) = nullptr;
  void (*quit)() = nullptr;
  void* win = nullptr;
  void* ren = nullptr;
  void* tex = nullptr;
  int tex_w = 0;
  int tex_h = 0;

  bool Load() {
    if (!std::getenv("DISPLAY")) return false;
    lib = dlopen("libSDL2-2.0.so.0", RTLD_LAZY);
    if (!lib) lib = dlopen("libSDL2.so", RTLD_LAZY);
    if (!lib) return false;
    init = reinterpret_cast<int (*)(uint32_t)>(dlsym(lib, "SDL_Init"));
    window = reinterpret_cast<void* (*)(const char*, int, int, int, int, uint32_t)>(
        dlsym(lib, "SDL_CreateWindow"));
    renderer = reinterpret_cast<void* (*)(void*, int, uint32_t)>(dlsym(lib, "SDL_CreateRenderer"));
    texture = reinterpret_cast<void* (*)(void*, uint32_t, int, int, int)>(dlsym(lib, "SDL_CreateTexture"));
    update = reinterpret_cast<int (*)(void*, const void*, const void*, int)>(dlsym(lib, "SDL_UpdateTexture"));
    clear = reinterpret_cast<int (*)(void*)>(dlsym(lib, "SDL_RenderClear"));
    copy = reinterpret_cast<int (*)(void*, void*, const void*, const void*)>(dlsym(lib, "SDL_RenderCopy"));
    present = reinterpret_cast<void (*)(void*)>(dlsym(lib, "SDL_RenderPresent"));
    poll = reinterpret_cast<int (*)(void*)>(dlsym(lib, "SDL_PollEvent"));
    destroy_texture = reinterpret_cast<void (*)(void*)>(dlsym(lib, "SDL_DestroyTexture"));
    destroy_renderer = reinterpret_cast<void (*)(void*)>(dlsym(lib, "SDL_DestroyRenderer"));
    destroy_window = reinterpret_cast<void (*)(void*)>(dlsym(lib, "SDL_DestroyWindow"));
    quit = reinterpret_cast<void (*)()>(dlsym(lib, "SDL_Quit"));
    return init && window && renderer && texture && update && clear && copy && present && quit;
  }

  bool Start() {
    if (init(kSdlInitVideo) != 0) return false;
    win = window("WebRTCSays remote", kSdlWindowPosCentered, kSdlWindowPosCentered, 1280, 720,
                 kSdlWindowShown);
    if (!win) return false;
    ren = renderer(win, -1, 0);
    if (!ren) return false;
    std::fprintf(stderr, "remote window: sdl desktop window on DISPLAY\n");
    return true;
  }

  bool Present(const uint8_t* rgba, int width, int height) {
    if (!ren) return false;
    if (!tex || tex_w != width || tex_h != height) {
      if (tex) destroy_texture(tex);
      tex = texture(ren, kSdlPixelFormatAbgr8888, 1, width, height);
      tex_w = width;
      tex_h = height;
    }
    if (!tex) return false;
    if (update(tex, nullptr, rgba, width * 4) != 0) return false;
    clear(ren);
    copy(ren, tex, nullptr, nullptr);
    present(ren);
    if (poll) {
      unsigned char event[256];
      while (poll(event)) {
      }
    }
    return true;
  }

  void Stop() {
    if (tex && destroy_texture) destroy_texture(tex);
    if (ren && destroy_renderer) destroy_renderer(ren);
    if (win && destroy_window) destroy_window(win);
    if (quit) quit();
    tex = ren = win = nullptr;
    if (lib) {
      dlclose(lib);
      lib = nullptr;
    }
  }
};

void I420ToRgba(const webrtc::I420BufferInterface& src,
                int dst_w,
                int dst_h,
                std::vector<uint8_t>& rgba) {
  rgba.resize(static_cast<size_t>(dst_w) * dst_h * 4);
  for (int y = 0; y < dst_h; ++y) {
    const int sy = y * src.height() / dst_h;
    const uint8_t* y_row = src.DataY() + sy * src.StrideY();
    const uint8_t* u_row = src.DataU() + (sy / 2) * src.StrideU();
    const uint8_t* v_row = src.DataV() + (sy / 2) * src.StrideV();
    uint8_t* out = rgba.data() + static_cast<size_t>(y) * dst_w * 4;
    for (int x = 0; x < dst_w; ++x) {
      const int sx = x * src.width() / dst_w;
      const int c = y_row[sx] - 16;
      const int d = u_row[sx / 2] - 128;
      const int e = v_row[sx / 2] - 128;
      const int r = (298 * c + 409 * e + 128) >> 8;
      const int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
      const int b = (298 * c + 516 * d + 128) >> 8;
      out[x * 4] = static_cast<uint8_t>(r < 0 ? 0 : (r > 255 ? 255 : r));
      out[x * 4 + 1] = static_cast<uint8_t>(g < 0 ? 0 : (g > 255 ? 255 : g));
      out[x * 4 + 2] = static_cast<uint8_t>(b < 0 ? 0 : (b > 255 ? 255 : b));
      out[x * 4 + 3] = 255;
    }
  }
}

class Presenter : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  Presenter(RemoteWindowChoice choice, bool openvr_ready, bool sdl_ready, bool sdl_fallback)
      : choice_(choice),
        openvr_ready_(openvr_ready),
        sdl_ready_(sdl_ready),
        sdl_fallback_(sdl_fallback) {
    thread_ = std::thread([this] { Loop(); });
  }

  ~Presenter() override {
    stopping_.store(true);
    if (thread_.joinable()) thread_.join();
  }

  void OnFrame(const webrtc::VideoFrame& frame) override {
    if (stopping_.load()) return;
    auto i420 = frame.video_frame_buffer()->ToI420();
    if (!i420) return;
    const int src_w = i420->width();
    const int src_h = i420->height();
    if (src_w <= 0 || src_h <= 0 || src_w > 4096 || src_h > 4096) return;
    int dst_w = src_w;
    int dst_h = src_h;
    if (dst_w > kMaxEdge || dst_h > kMaxEdge) {
      if (dst_w * 9 >= dst_h * 16) {
        dst_w = kMaxEdge;
        dst_h = std::max(2, src_h * kMaxEdge / src_w);
      } else {
        dst_h = kMaxEdge;
        dst_w = std::max(2, src_w * kMaxEdge / src_h);
      }
      dst_w &= ~1;
      dst_h &= ~1;
    }
    std::vector<uint8_t> rgba;
    I420ToRgba(*i420, dst_w, dst_h, rgba);
    std::lock_guard<std::mutex> lock(mu_);
    rgba_.swap(rgba);
    width_ = dst_w;
    height_ = dst_h;
    src_width_ = src_w;
    src_height_ = src_h;
    dirty_ = true;
    ++received_;
  }

 private:
  void Loop() {
    OpenVrFns openvr;
    SdlFns sdl;
    RemoteWindowChoice active = RemoteWindowChoice::kNone;
    if (choice_ == RemoteWindowChoice::kOpenVr && openvr_ready_ && openvr.Load() &&
        openvr.Start()) {
      active = RemoteWindowChoice::kOpenVr;
    }
    if (active == RemoteWindowChoice::kNone && sdl_ready_ &&
        (choice_ == RemoteWindowChoice::kSdl ||
         (choice_ == RemoteWindowChoice::kOpenVr && sdl_fallback_))) {
      if (choice_ == RemoteWindowChoice::kOpenVr) {
        std::fprintf(stderr, "openvr overlay unavailable; trying an SDL desktop window\n");
      }
      if (sdl.Load() && sdl.Start()) active = RemoteWindowChoice::kSdl;
    }
    if (active == RemoteWindowChoice::kNone) {
      std::fprintf(stderr, "remote window not shown (%s)\n", RemoteWindowName(choice_));
    }
    auto next_log = std::chrono::steady_clock::now();
    while (!stopping_.load()) {
      std::vector<uint8_t> rgba;
      int width = 0;
      int height = 0;
      int src_w = 0;
      int src_h = 0;
      int received = 0;
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (dirty_) {
          rgba.swap(rgba_);
          width = width_;
          height = height_;
          src_w = src_width_;
          src_h = src_height_;
          dirty_ = false;
        }
        received = received_;
      }
      if (!rgba.empty() && active == RemoteWindowChoice::kOpenVr) {
        if (openvr.Present(rgba.data(), width, height)) ++presented_;
      } else if (!rgba.empty() && active == RemoteWindowChoice::kSdl) {
        if (sdl.Present(rgba.data(), width, height)) ++presented_;
      }
      const auto now = std::chrono::steady_clock::now();
      if (received > 0 && now >= next_log) {
        std::fprintf(stderr,
                     "remote video: shown=%s received=%dx%d display=%dx%d frames=%d presented=%d\n",
                     RemoteWindowName(active), src_w, src_h, width, height, received, presented_);
        next_log = now + std::chrono::seconds(5);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    openvr.Stop();
    sdl.Stop();
  }

  RemoteWindowChoice choice_;
  bool openvr_ready_;
  bool sdl_ready_;
  bool sdl_fallback_;
  std::mutex mu_;
  std::vector<uint8_t> rgba_;
  int width_ = 0;
  int height_ = 0;
  int src_width_ = 0;
  int src_height_ = 0;
  bool dirty_ = false;
  int received_ = 0;
  int presented_ = 0;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
};

bool OpenVrLibraryPresent() {
  void* lib = dlopen("libopenvr_api.so", RTLD_LAZY);
  if (!lib) lib = dlopen("/opt/steamvr/bin/linuxarm64/libopenvr_api.so", RTLD_LAZY);
  if (!lib) return false;
  const bool ok = dlsym(lib, "VR_InitInternal") && dlsym(lib, "VR_GetGenericInterface");
  dlclose(lib);
  return ok;
}

bool SdlLibraryPresent() {
  if (!std::getenv("DISPLAY")) return false;
  void* lib = dlopen("libSDL2-2.0.so.0", RTLD_LAZY);
  if (!lib) lib = dlopen("libSDL2.so", RTLD_LAZY);
  if (!lib) return false;
  const bool ok = dlsym(lib, "SDL_CreateWindow") && dlsym(lib, "SDL_CreateRenderer");
  dlclose(lib);
  return ok;
}

}  // namespace

std::unique_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> StartRemoteVideoWindow(
    const std::string& requested) {
  const bool openvr = OpenVrLibraryPresent();
  const bool sdl = SdlLibraryPresent();
  const RemoteWindowChoice choice = ChooseRemoteWindow(requested, openvr, sdl);
  if (choice == RemoteWindowChoice::kNone) {
    std::fprintf(stderr, "remote window not shown (%s)\n", requested.c_str());
    return nullptr;
  }
  return std::make_unique<Presenter>(choice, openvr, sdl, requested == "auto");
}
