/*
 *  (c) 2026, wilddolphin2025
 *  For WebRTCsays.ai project
 */

#ifndef EXAMPLES_DIRECT_FRAME_FIT_H_
#define EXAMPLES_DIRECT_FRAME_FIT_H_

#include <algorithm>
#include <cstdint>

// Content rectangle of src fitted inside box. One scale for both axes.
// A 16:9 source in a 16:9 box fills it. Other ratios leave a centered margin.
struct FrameFit {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

inline FrameFit FitInside(int src_w, int src_h, int box_w, int box_h) {
  FrameFit fit;
  if (src_w <= 0 || src_h <= 0 || box_w <= 0 || box_h <= 0) return fit;
  if (static_cast<int64_t>(src_w) * box_h <= static_cast<int64_t>(src_h) * box_w) {
    fit.height = box_h;
    fit.width = static_cast<int>(static_cast<int64_t>(src_w) * box_h / src_h);
  } else {
    fit.width = box_w;
    fit.height = static_cast<int>(static_cast<int64_t>(src_h) * box_w / src_w);
  }
  fit.width = std::min(box_w, std::max(1, fit.width));
  fit.height = std::min(box_h, std::max(1, fit.height));
  fit.x = (box_w - fit.width) / 2;
  fit.y = (box_h - fit.height) / 2;
  return fit;
}

#endif  // EXAMPLES_DIRECT_FRAME_FIT_H_
