#pragma once

#include "capture.hpp"

namespace t3render {
// Output is <trace-path>.frame-<frame>.ppm. Existing files are never replaced.
FrameImageResult ReadFrameImage(IDirect3DDevice8*, const std::wstring&, std::uint64_t);
}
