/*
 *  (c) 2026, wilddolphin2025
 *  For WebRTCsays.ai project
 */

#include "still_image.h"

#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>

namespace {

constexpr int kMaxEdge = 8192;
constexpr size_t kMaxFileBytes = 32 * 1024 * 1024;

struct ZStream {
  uint8_t* next_in;
  uint32_t avail_in;
  uint32_t pad_in;
  uint64_t total_in;
  uint8_t* next_out;
  uint32_t avail_out;
  uint32_t pad_out;
  uint64_t total_out;
  char* msg;
  void* state;
  void* zalloc;
  void* zfree;
  void* opaque;
  int data_type;
  uint32_t pad_adler;
  uint64_t adler;
  uint64_t reserved;
};

static_assert(sizeof(ZStream) == 112, "zlib z_stream layout");

using InflateInitFn = int (*)(ZStream*, const char*, int);
using InflateFn = int (*)(ZStream*, int);
using InflateEndFn = int (*)(ZStream*);

struct Zlib {
  void* lib = nullptr;
  InflateInitFn init = nullptr;
  InflateFn inflate = nullptr;
  InflateEndFn end = nullptr;

  bool Open() {
    lib = dlopen("libz.so.1", RTLD_LAZY);
    if (!lib) return false;
    init = reinterpret_cast<InflateInitFn>(dlsym(lib, "inflateInit_"));
    inflate = reinterpret_cast<InflateFn>(dlsym(lib, "inflate"));
    end = reinterpret_cast<InflateEndFn>(dlsym(lib, "inflateEnd"));
    return init && inflate && end;
  }

  ~Zlib() {
    if (lib) dlclose(lib);
  }
};

uint32_t ReadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

int Paeth(int a, int b, int c) {
  const int p = a + b - c;
  const int pa = std::abs(p - a);
  const int pb = std::abs(p - b);
  const int pc = std::abs(p - c);
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

bool Unfilter(std::vector<uint8_t>& raw, int width, int channels, std::string& error) {
  const int stride = width * channels;
  const int row_bytes = stride + 1;
  if (width <= 0 || raw.size() % static_cast<size_t>(row_bytes) != 0) {
    error = "png inflate size does not match the image";
    return false;
  }
  std::vector<uint8_t> out(static_cast<size_t>(raw.size() / row_bytes) * stride);
  const int height = static_cast<int>(out.size() / stride);
  for (int y = 0; y < height; ++y) {
    const uint8_t filter = raw[static_cast<size_t>(y) * row_bytes];
    const uint8_t* src = raw.data() + static_cast<size_t>(y) * row_bytes + 1;
    uint8_t* dst = out.data() + static_cast<size_t>(y) * stride;
    const uint8_t* prev = y == 0 ? nullptr : dst - stride;
    for (int x = 0; x < stride; ++x) {
      const int left = x >= channels ? dst[x - channels] : 0;
      const int up = prev ? prev[x] : 0;
      const int up_left = (prev && x >= channels) ? prev[x - channels] : 0;
      int value = src[x];
      if (filter == 1) value += left;
      else if (filter == 2) value += up;
      else if (filter == 3) value += (left + up) / 2;
      else if (filter == 4) value += Paeth(left, up, up_left);
      else if (filter != 0) {
        error = "png filter is not supported";
        return false;
      }
      dst[x] = static_cast<uint8_t>(value & 0xff);
    }
  }
  raw.swap(out);
  return true;
}

void RgbToYuv(uint8_t r, uint8_t g, uint8_t b, uint8_t& y, uint8_t& u, uint8_t& v) {
  y = static_cast<uint8_t>((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
  u = static_cast<uint8_t>((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
  v = static_cast<uint8_t>((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
}

}  // namespace

bool LoadPngRgb(const std::string& path, RgbImage& out, std::string& error) {
  out = RgbImage();
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "image file could not be opened";
    return false;
  }
  file.seekg(0, std::ios::end);
  const std::streamoff length = file.tellg();
  if (length < 8 || static_cast<size_t>(length) > kMaxFileBytes) {
    error = "image file is empty or larger than 32MB";
    return false;
  }
  file.seekg(0, std::ios::beg);
  std::vector<uint8_t> bytes(static_cast<size_t>(length));
  file.read(reinterpret_cast<char*>(bytes.data()), length);
  if (!file) {
    error = "image file could not be read";
    return false;
  }
  const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (std::memcmp(bytes.data(), sig, 8) != 0) {
    error = "image file is not a PNG";
    return false;
  }

  int width = 0;
  int height = 0;
  int color = -1;
  std::vector<uint8_t> idat;
  size_t pos = 8;
  bool saw_ihdr = false;
  bool ended = false;
  while (pos + 12 <= bytes.size() && !ended) {
    const uint32_t chunk_len = ReadBe32(bytes.data() + pos);
    if (pos + 12 + chunk_len > bytes.size()) {
      error = "png chunk is truncated";
      return false;
    }
    const uint8_t* type = bytes.data() + pos + 4;
    const uint8_t* data = bytes.data() + pos + 8;
    if (std::memcmp(type, "IHDR", 4) == 0) {
      if (chunk_len < 13) {
        error = "png header is truncated";
        return false;
      }
      width = static_cast<int>(ReadBe32(data));
      height = static_cast<int>(ReadBe32(data + 4));
      const int depth = data[8];
      color = data[9];
      const int interlace = data[12];
      if (depth != 8 || (color != 0 && color != 2 && color != 6) || data[10] != 0 ||
          data[11] != 0) {
        error = "png must be 8-bit gray, RGB, or RGBA";
        return false;
      }
      if (interlace != 0) {
        error = "interlaced png is not supported";
        return false;
      }
      if (width <= 0 || height <= 0 || width > kMaxEdge || height > kMaxEdge) {
        error = "png dimensions are out of range";
        return false;
      }
      saw_ihdr = true;
    } else if (std::memcmp(type, "IDAT", 4) == 0) {
      idat.insert(idat.end(), data, data + chunk_len);
    } else if (std::memcmp(type, "IEND", 4) == 0) {
      ended = true;
    }
    pos += 12 + chunk_len;
  }
  if (!saw_ihdr || idat.empty()) {
    error = "png is missing image data";
    return false;
  }

  const int channels = color == 0 ? 1 : (color == 2 ? 3 : 4);
  const size_t expected = static_cast<size_t>(height) * (static_cast<size_t>(width) * channels + 1);
  if (expected > 64u * 1024u * 1024u) {
    error = "png is too large";
    return false;
  }
  Zlib zlib;
  if (!zlib.Open()) {
    error = "libz.so.1 is not available to decode the png";
    return false;
  }
  std::vector<uint8_t> raw(expected);
  ZStream stream;
  std::memset(&stream, 0, sizeof(stream));
  stream.next_in = idat.data();
  stream.avail_in = static_cast<uint32_t>(idat.size());
  stream.next_out = raw.data();
  stream.avail_out = static_cast<uint32_t>(raw.size());
  if (zlib.init(&stream, "1.2.11", static_cast<int>(sizeof(stream))) != 0) {
    error = "png inflate could not start";
    return false;
  }
  const int inflate_result = zlib.inflate(&stream, 4);
  zlib.end(&stream);
  if (inflate_result != 1 || stream.total_out != expected) {
    error = "png inflate did not produce the expected image";
    return false;
  }
  if (!Unfilter(raw, width, channels, error)) return false;

  out.width = width;
  out.height = height;
  out.rgb.resize(static_cast<size_t>(width) * height * 3);
  for (int i = 0; i < width * height; ++i) {
    const uint8_t* p = raw.data() + static_cast<size_t>(i) * channels;
    if (channels == 1) {
      out.rgb[static_cast<size_t>(i) * 3] = p[0];
      out.rgb[static_cast<size_t>(i) * 3 + 1] = p[0];
      out.rgb[static_cast<size_t>(i) * 3 + 2] = p[0];
    } else {
      out.rgb[static_cast<size_t>(i) * 3] = p[0];
      out.rgb[static_cast<size_t>(i) * 3 + 1] = p[1];
      out.rgb[static_cast<size_t>(i) * 3 + 2] = p[2];
    }
  }
  return true;
}

bool LetterboxToI420(const RgbImage& src,
                     int dst_width,
                     int dst_height,
                     LetterboxedI420& out,
                     std::string& error) {
  out = LetterboxedI420();
  if (src.width <= 0 || src.height <= 0 ||
      src.rgb.size() < static_cast<size_t>(src.width) * src.height * 3) {
    error = "decoded image is empty";
    return false;
  }
  if (dst_width < 2 || dst_height < 2) {
    error = "output size is too small";
    return false;
  }
  dst_width &= ~1;
  dst_height &= ~1;
  int content_w = 0;
  int content_h = 0;
  if (static_cast<int64_t>(src.width) * dst_height <= static_cast<int64_t>(src.height) * dst_width) {
    content_h = dst_height;
    content_w = static_cast<int>(static_cast<int64_t>(src.width) * dst_height / src.height);
  } else {
    content_w = dst_width;
    content_h = static_cast<int>(static_cast<int64_t>(src.height) * dst_width / src.width);
  }
  content_w &= ~1;
  content_h &= ~1;
  if (content_w < 2) content_w = 2;
  if (content_h < 2) content_h = 2;
  const int origin_x = ((dst_width - content_w) / 2) & ~1;
  const int origin_y = ((dst_height - content_h) / 2) & ~1;

  const int y_size = dst_width * dst_height;
  const int c_w = dst_width / 2;
  const int c_h = dst_height / 2;
  const int c_size = c_w * c_h;
  out.i420.assign(static_cast<size_t>(y_size + c_size * 2), 0);
  uint8_t* y_plane = out.i420.data();
  uint8_t* u_plane = y_plane + y_size;
  uint8_t* v_plane = u_plane + c_size;
  std::memset(y_plane, 16, y_size);
  std::memset(u_plane, 128, c_size);
  std::memset(v_plane, 128, c_size);

  for (int y = 0; y < content_h; ++y) {
    const int sy = y * src.height / content_h;
    const int dy = origin_y + y;
    for (int x = 0; x < content_w; ++x) {
      const int sx = x * src.width / content_w;
      const uint8_t* p = src.rgb.data() + (static_cast<size_t>(sy) * src.width + sx) * 3;
      uint8_t yy, uu, vv;
      RgbToYuv(p[0], p[1], p[2], yy, uu, vv);
      y_plane[dy * dst_width + origin_x + x] = yy;
      if ((dy % 2) == 0 && (x % 2) == 0) {
        const int cx = (origin_x + x) / 2;
        const int cy = dy / 2;
        u_plane[cy * c_w + cx] = uu;
        v_plane[cy * c_w + cx] = vv;
      }
    }
  }

  out.src_width = src.width;
  out.src_height = src.height;
  out.dst_width = dst_width;
  out.dst_height = dst_height;
  out.content_width = content_w;
  out.content_height = content_h;
  out.origin_x = origin_x;
  out.origin_y = origin_y;
  return true;
}
