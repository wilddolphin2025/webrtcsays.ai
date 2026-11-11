#include "remote_choice.h"
#include "still_image.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int failures = 0;

void Expect(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s\n", message);
    ++failures;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: still_image_test <square.png> <wide.png>\n");
    return 2;
  }
  RgbImage square;
  std::string error;
  Expect(LoadPngRgb(argv[1], square, error), error.c_str());
  Expect(square.width == 2 && square.height == 2, "square size");
  Expect(square.rgb.size() >= 3 && square.rgb[0] == 255 && square.rgb[1] == 0, "square red");

  LetterboxedI420 boxed;
  Expect(LetterboxToI420(square, 8, 4, boxed, error), error.c_str());
  Expect(boxed.content_width == 4 && boxed.content_height == 4, "square letterbox content");
  Expect(boxed.origin_x == 2 && boxed.origin_y == 0, "square letterbox origin");
  const uint8_t* y_plane = boxed.i420.data();
  Expect(y_plane[0] == 16, "letterbox bar is black");
  const int content_y = y_plane[boxed.origin_y * boxed.dst_width + boxed.origin_x];
  Expect(content_y > 40, "letterbox content is not a black bar");

  RgbImage wide;
  Expect(LoadPngRgb(argv[2], wide, error), error.c_str());
  LetterboxedI420 exact;
  Expect(LetterboxToI420(wide, 4, 2, exact, error), error.c_str());
  Expect(exact.content_width == 4 && exact.content_height == 2, "exact fit");
  Expect(exact.origin_x == 0 && exact.origin_y == 0, "exact origin");

  RgbImage rejected;
  Expect(!LoadPngRgb("/dev/null", rejected, error), "non-png rejected");

  Expect(ChooseRemoteWindow("auto", true, false) == RemoteWindowChoice::kOpenVr, "auto openvr");
  Expect(ChooseRemoteWindow("auto", false, true) == RemoteWindowChoice::kSdl, "auto sdl");
  Expect(ChooseRemoteWindow("auto", false, false) == RemoteWindowChoice::kNone, "auto none");
  Expect(ChooseRemoteWindow("openvr", false, true) == RemoteWindowChoice::kNone,
         "openvr miss is not an sdl panel");
  Expect(ChooseRemoteWindow("sdl", true, false) == RemoteWindowChoice::kNone, "sdl needs a display");
  Expect(ChooseRemoteWindow("none", true, true) == RemoteWindowChoice::kNone, "none stays closed");

  if (failures) {
    std::fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  std::printf("still image and window choice ok\n");
  return 0;
}
