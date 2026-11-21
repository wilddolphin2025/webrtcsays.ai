#include "frame_fit.h"

#include <cstdio>

namespace {

int failures = 0;

void Expect(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s\n", message);
    ++failures;
  }
}

}  // namespace

int main() {
  const FrameFit hd = FitInside(640, 360, 1920, 1080);
  Expect(hd.x == 0 && hd.y == 0 && hd.width == 1920 && hd.height == 1080, "640x360 fills 16:9");
  const FrameFit exact = FitInside(1920, 1080, 1920, 1080);
  Expect(exact.x == 0 && exact.y == 0 && exact.width == 1920 && exact.height == 1080,
         "1920x1080 fills");
  const FrameFit four = FitInside(640, 480, 1920, 1080);
  Expect(four.width == 1440 && four.height == 1080 && four.x == 240 && four.y == 0, "4:3 side bars");
  const FrameFit old_slot = FitInside(640, 360, 1920, 820);
  Expect(old_slot.width == 1457 && old_slot.height == 820 && old_slot.x == 231 && old_slot.y == 0,
         "820-tall slot keeps 16:9 instead of stretching");
  if (failures) {
    std::fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  std::printf("frame fit ok\n");
  return 0;
}
