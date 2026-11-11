#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace d3d12_probe {
enum class FenceState { Waiting, Complete, DeviceRemoved };
inline FenceState EvaluateFence(std::uint64_t completed, std::uint64_t target) {
    if (completed == std::numeric_limits<std::uint64_t>::max()) return FenceState::DeviceRemoved;
    return completed < target ? FenceState::Waiting : FenceState::Complete;
}

struct PixelStats {
    std::uint64_t fnv1a = UINT64_C(14695981039346656037);
    std::size_t red_pixels = 0;
    bool center_red = false;
    bool corners_black = false;
};

// Hash only RGBA pixels, not driver-specific row padding. Fail closed on
// invalid buffer layouts before reading any bytes.
inline bool AnalyzePixels(const std::uint8_t* data, std::size_t size, std::size_t offset,
                          std::size_t pitch, std::size_t width, std::size_t height, PixelStats& result) {
    if (!data || !width || !height || width > std::numeric_limits<std::size_t>::max() / 4) return false;
    const std::size_t row_bytes = width * 4;
    if (pitch < row_bytes || offset > size || row_bytes > size - offset) return false;
    if (height - 1 > (size - offset - row_bytes) / pitch) return false;
    auto pixel = [&](std::size_t x, std::size_t y) { return data + offset + y * pitch + x * 4; };
    auto red = [](const std::uint8_t* p) { return p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255; };
    auto black = [](const std::uint8_t* p) { return p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 255; };
    PixelStats stats;
    stats.center_red = red(pixel(width / 2, height / 2));
    stats.corners_black = black(pixel(0, 0)) && black(pixel(width - 1, height - 1));
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            const auto* p = pixel(x, y);
            if (red(p)) ++stats.red_pixels;
            for (int channel = 0; channel < 4; ++channel) {
                stats.fnv1a ^= p[channel];
                stats.fnv1a *= UINT64_C(1099511628211);
            }
        }
    }
    result = stats;
    return true;
}

inline bool AnalyzeTriangle256(const std::uint8_t* data, std::size_t size, std::size_t offset,
                               std::size_t pitch, PixelStats& result) {
    if (!AnalyzePixels(data, size, offset, pitch, 256, 256, result)) return false;
    auto pixel = [&](std::size_t x, std::size_t y) { return data + offset + y * pitch + x * 4; };
    auto red = [](const std::uint8_t* p) { return p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255; };
    auto black = [](const std::uint8_t* p) { return p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 255; };
    return result.center_red && result.corners_black && black(pixel(255, 0)) && black(pixel(0, 255)) &&
           red(pixel(128, 40)) && red(pixel(64, 190)) && red(pixel(192, 190)) &&
           black(pixel(40, 50)) && black(pixel(215, 50)) && black(pixel(128, 240)) &&
           result.red_pixels > 1000 && result.red_pixels < 256 * 256;
}
}  // namespace d3d12_probe
