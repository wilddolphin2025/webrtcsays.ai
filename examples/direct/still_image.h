/*
 *  (c) 2026, wilddolphin2025
 *  For WebRTCsays.ai project
 */

#ifndef EXAMPLES_DIRECT_STILL_IMAGE_H_
#define EXAMPLES_DIRECT_STILL_IMAGE_H_

#include <cstdint>
#include <string>
#include <vector>

// Decoded 8-bit RGB. This is a still picture, not a screen capture.
struct RgbImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgb;
};

// Letterboxed I420 at an even destination size. Bars are black.
struct LetterboxedI420 {
  int src_width = 0;
  int src_height = 0;
  int dst_width = 0;
  int dst_height = 0;
  int content_width = 0;
  int content_height = 0;
  int origin_x = 0;
  int origin_y = 0;
  std::vector<uint8_t> i420;
};

// PNG only: 8-bit gray, RGB, or RGBA, non-interlaced. Fails closed on
// anything else so a pattern is not substituted for the requested file.
bool LoadPngRgb(const std::string& path, RgbImage& out, std::string& error);

bool LetterboxToI420(const RgbImage& src,
                     int dst_width,
                     int dst_height,
                     LetterboxedI420& out,
                     std::string& error);

#endif  // EXAMPLES_DIRECT_STILL_IMAGE_H_
