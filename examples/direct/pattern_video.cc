/*
 *  (c) 2026, wilddolphin2025
 *  For WebRTCsays.ai project
 */

#include "pattern_video.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <thread>
#include <utility>
#include <vector>

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

namespace {

void PaintPattern(webrtc::I420Buffer* buf, int frame_index) {
  static const uint8_t kY[8] = {235, 210, 170, 145, 106, 81, 41, 16};
  static const uint8_t kU[8] = {128, 16, 166, 54, 202, 90, 240, 128};
  static const uint8_t kV[8] = {128, 146, 16, 34, 222, 240, 110, 128};
  const int width = buf->width();
  const int height = buf->height();
  const int bar = std::max(1, width / 8);
  uint8_t* y_plane = buf->MutableDataY();
  uint8_t* u_plane = buf->MutableDataU();
  uint8_t* v_plane = buf->MutableDataV();
  const int y_stride = buf->StrideY();
  const int u_stride = buf->StrideU();
  const int v_stride = buf->StrideV();

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int band = std::min(7, x / bar);
      y_plane[y * y_stride + x] = kY[band];
    }
  }
  const int cw = buf->ChromaWidth();
  const int ch = buf->ChromaHeight();
  for (int y = 0; y < ch; ++y) {
    for (int x = 0; x < cw; ++x) {
      const int band = std::min(7, (x * 2) / bar);
      u_plane[y * u_stride + x] = kU[band];
      v_plane[y * v_stride + x] = kV[band];
    }
  }

  const int box = std::max(32, width / 24);
  const int travel = std::max(1, width - box);
  const int x0 = (frame_index * std::max(1, width / 120)) % travel;
  const int y0 = height / 3;
  for (int y = y0; y < std::min(height, y0 + box); ++y) {
    std::memset(y_plane + y * y_stride + x0, 235, std::min(box, width - x0));
  }
}

struct X11 {
  void* lib = nullptr;
  void* (*open_display)(const char*) = nullptr;
  int (*close_display)(void*) = nullptr;
  unsigned long (*root)(void*, int) = nullptr;
  int (*default_screen)(void*) = nullptr;
  int (*display_width)(void*, int) = nullptr;
  int (*display_height)(void*, int) = nullptr;
  void* (*get_image)(void*, unsigned long, int, int, unsigned int, unsigned int,
                     unsigned long, int) = nullptr;
  int (*destroy_image)(void*) = nullptr;
  int (*init_threads)(void) = nullptr;

  bool Open() {
    lib = dlopen("libX11.so.6", RTLD_LAZY);
    if (!lib) return false;
    open_display = reinterpret_cast<void* (*)(const char*)>(dlsym(lib, "XOpenDisplay"));
    close_display = reinterpret_cast<int (*)(void*)>(dlsym(lib, "XCloseDisplay"));
    root = reinterpret_cast<unsigned long (*)(void*, int)>(dlsym(lib, "XRootWindow"));
    default_screen = reinterpret_cast<int (*)(void*)>(dlsym(lib, "XDefaultScreen"));
    display_width = reinterpret_cast<int (*)(void*, int)>(dlsym(lib, "XDisplayWidth"));
    display_height = reinterpret_cast<int (*)(void*, int)>(dlsym(lib, "XDisplayHeight"));
    get_image = reinterpret_cast<void* (*)(void*, unsigned long, int, int, unsigned int,
                                           unsigned int, unsigned long, int)>(
        dlsym(lib, "XGetImage"));
    destroy_image = reinterpret_cast<int (*)(void*)>(dlsym(lib, "XDestroyImage"));
    init_threads = reinterpret_cast<int (*)(void)>(dlsym(lib, "XInitThreads"));
    return open_display && close_display && root && default_screen && display_width &&
           display_height && get_image && destroy_image;
  }

  ~X11() {
    if (lib) dlclose(lib);
  }
};

struct XImageView {
  int width;
  int height;
  int xoffset;
  int format;
  char* data;
  int byte_order;
  int bitmap_unit;
  int bitmap_bit_order;
  int bitmap_pad;
  int depth;
  int bytes_per_line;
  int bits_per_pixel;
  unsigned long red_mask;
  unsigned long green_mask;
  unsigned long blue_mask;
};

int MaskShift(unsigned long mask) {
  int shift = 0;
  if (mask == 0) return 0;
  while ((mask & 1UL) == 0) {
    mask >>= 1;
    ++shift;
  }
  return shift;
}

bool GrabScreen(X11& x11, void* display, webrtc::I420Buffer* buf) {
  const int screen = x11.default_screen(display);
  const int sw = x11.display_width(display, screen);
  const int sh = x11.display_height(display, screen);
  if (sw <= 0 || sh <= 0) return false;
  // ZPixmap == 2.
  void* image = x11.get_image(display, x11.root(display, screen), 0, 0,
                              static_cast<unsigned int>(sw),
                              static_cast<unsigned int>(sh), ~0UL, 2);
  if (!image) return false;
  auto* img = reinterpret_cast<XImageView*>(image);
  const bool ok = img->data && (img->bits_per_pixel == 24 || img->bits_per_pixel == 32);
  if (ok) {
    const int rs = MaskShift(img->red_mask);
    const int gs = MaskShift(img->green_mask);
    const int bs = MaskShift(img->blue_mask);
    const int bpp = img->bits_per_pixel / 8;
    const int dw = buf->width();
    const int dh = buf->height();
    uint8_t* y_plane = buf->MutableDataY();
    uint8_t* u_plane = buf->MutableDataU();
    uint8_t* v_plane = buf->MutableDataV();
    for (int y = 0; y < dh; ++y) {
      const int sy = y * sh / dh;
      for (int x = 0; x < dw; ++x) {
        const int sx = x * sw / dw;
        const uint8_t* p = reinterpret_cast<uint8_t*>(img->data) +
                           sy * img->bytes_per_line + sx * bpp;
        unsigned long pixel = 0;
        for (int b = 0; b < bpp; ++b) pixel |= static_cast<unsigned long>(p[b]) << (8 * b);
        const int r = (pixel >> rs) & 0xff;
        const int g = (pixel >> gs) & 0xff;
        const int b = (pixel >> bs) & 0xff;
        y_plane[y * buf->StrideY() + x] = static_cast<uint8_t>((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
        if ((y % 2) == 0 && (x % 2) == 0) {
          const int cx = x / 2;
          const int cy = y / 2;
          u_plane[cy * buf->StrideU() + cx] =
              static_cast<uint8_t>((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
          v_plane[cy * buf->StrideV() + cx] =
              static_cast<uint8_t>((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
        }
      }
    }
  }
  x11.destroy_image(image);
  return ok;
}

class Pump : public DirectVideoPump {
 public:
  Pump(webrtc::scoped_refptr<webrtc::FakeVideoTrackSource> source,
       int width,
       int height,
       int fps,
       bool screen)
      : source_(std::move(source)),
        width_(width),
        height_(height),
        fps_(std::max(1, fps)),
        want_screen_(screen) {}

  ~Pump() override { Stop(); }

  void Stop() override {
    if (running_.exchange(false)) {
      if (thread_.joinable()) thread_.join();
    }
  }

  void Start() {
    running_ = true;
    thread_ = std::thread([this] { Loop(); });
  }

 private:
  void Loop() {
    X11 x11;
    void* display = nullptr;
    bool use_screen = false;
    if (want_screen_ && x11.Open()) {
      if (x11.init_threads) x11.init_threads();
      display = x11.open_display(nullptr);
      use_screen = display != nullptr;
      if (!use_screen) {
        fprintf(stderr, "screen capture unavailable (no DISPLAY); using test pattern\n");
      } else {
        fprintf(stderr, "screen capture: X11 root window scaled to %dx%d\n", width_, height_);
      }
    } else if (want_screen_) {
      fprintf(stderr, "libX11.so.6 not loaded; using test pattern\n");
    }

    const auto interval = std::chrono::microseconds(1000000 / fps_);
    auto next = std::chrono::steady_clock::now();
    int frame_index = 0;
    while (running_.load()) {
      auto buffer = webrtc::I420Buffer::Create(width_, height_);
      bool painted = false;
      if (use_screen) {
        painted = GrabScreen(x11, display, buffer.get());
        if (!painted) {
          fprintf(stderr, "screen grab failed; switching to test pattern\n");
          use_screen = false;
        }
      }
      if (!painted) PaintPattern(buffer.get(), frame_index);
      webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                                     .set_video_frame_buffer(buffer)
                                     .set_timestamp_us(webrtc::TimeMicros())
                                     .set_rotation(webrtc::kVideoRotation_0)
                                     .build();
      source_->InjectFrame(frame);
      ++frame_index;
      next += interval;
      const auto now = std::chrono::steady_clock::now();
      if (now < next) {
        std::this_thread::sleep_until(next);
      } else {
        next = now;
      }
    }
    if (display) x11.close_display(display);
  }

  webrtc::scoped_refptr<webrtc::FakeVideoTrackSource> source_;
  int width_;
  int height_;
  int fps_;
  bool want_screen_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace

class StillPump : public DirectVideoPump {
 public:
  StillPump(webrtc::scoped_refptr<webrtc::FakeVideoTrackSource> source,
            std::vector<uint8_t> i420,
            int width,
            int height,
            int fps)
      : source_(std::move(source)),
        i420_(std::move(i420)),
        width_(width),
        height_(height),
        fps_(std::max(1, fps)) {}

  ~StillPump() override { Stop(); }

  void Stop() override {
    if (running_.exchange(false)) {
      if (thread_.joinable()) thread_.join();
    }
  }

  void Start() {
    running_ = true;
    thread_ = std::thread([this] { Loop(); });
  }

 private:
  void Loop() {
    const int y_size = width_ * height_;
    const int c_size = (width_ / 2) * (height_ / 2);
    const uint8_t* y_plane = i420_.data();
    const uint8_t* u_plane = y_plane + y_size;
    const uint8_t* v_plane = u_plane + c_size;
    const auto interval = std::chrono::microseconds(1000000 / fps_);
    auto next = std::chrono::steady_clock::now();
    while (running_.load()) {
      auto buffer = webrtc::I420Buffer::Copy(width_, height_, y_plane, width_, u_plane,
                                             width_ / 2, v_plane, width_ / 2);
      webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                                     .set_video_frame_buffer(buffer)
                                     .set_timestamp_us(webrtc::TimeMicros())
                                     .set_rotation(webrtc::kVideoRotation_0)
                                     .build();
      source_->InjectFrame(frame);
      next += interval;
      const auto now = std::chrono::steady_clock::now();
      if (now < next) std::this_thread::sleep_until(next);
      else next = now;
    }
  }

  webrtc::scoped_refptr<webrtc::FakeVideoTrackSource> source_;
  std::vector<uint8_t> i420_;
  int width_;
  int height_;
  int fps_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

std::unique_ptr<DirectVideoPump> StartDirectVideoPump(
    webrtc::scoped_refptr<webrtc::FakeVideoTrackSource> source,
    int width,
    int height,
    int fps,
    bool screen) {
  auto pump = std::make_unique<Pump>(std::move(source), width, height, fps, screen);
  pump->Start();
  return pump;
}

std::unique_ptr<DirectVideoPump> StartStillVideoPump(
    webrtc::scoped_refptr<webrtc::FakeVideoTrackSource> source,
    std::vector<uint8_t> i420,
    int width,
    int height,
    int fps) {
  auto pump = std::make_unique<StillPump>(std::move(source), std::move(i420), width, height, fps);
  pump->Start();
  return pump;
}
