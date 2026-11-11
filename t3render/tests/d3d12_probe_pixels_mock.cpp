#include "d3d12_probe_pixels.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <vector>

int main() {
    assert(d3d12_probe::EvaluateFence(0, 1) == d3d12_probe::FenceState::Waiting);
    assert(d3d12_probe::EvaluateFence(1, 1) == d3d12_probe::FenceState::Complete);
    assert(d3d12_probe::EvaluateFence(std::numeric_limits<std::uint64_t>::max(), 1) == d3d12_probe::FenceState::DeviceRemoved);
    constexpr std::size_t width = 4, height = 4, pitch = 20, offset = 8;
    std::vector<std::uint8_t> bytes(offset + pitch * height, 77);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            auto* p = bytes.data() + offset + y * pitch + x * 4;
            p[0] = p[1] = p[2] = 0; p[3] = 255;
        }
    }
    bytes[offset + 2 * pitch + 2 * 4] = 255;
    d3d12_probe::PixelStats before, after;
    assert(d3d12_probe::AnalyzePixels(bytes.data(), bytes.size(), offset, pitch, width, height, before));
    assert(before.center_red && before.corners_black && before.red_pixels == 1);
    bytes[offset + 4 * width] = 9;  // Padding does not influence pixel hash.
    assert(d3d12_probe::AnalyzePixels(bytes.data(), bytes.size(), offset, pitch, width, height, after));
    assert(before.fnv1a == after.fnv1a);
    assert(!d3d12_probe::AnalyzePixels(nullptr, bytes.size(), offset, pitch, width, height, after));
    assert(!d3d12_probe::AnalyzePixels(bytes.data(), bytes.size(), offset, 15, width, height, after));
    assert(!d3d12_probe::AnalyzePixels(bytes.data(), bytes.size(), offset, pitch, width, 5, after));
    assert(!d3d12_probe::AnalyzePixels(bytes.data(), bytes.size(), bytes.size(), pitch, width, height, after));
    assert(!d3d12_probe::AnalyzePixels(bytes.data(), bytes.size(), offset, pitch,
                                        std::numeric_limits<std::size_t>::max(), height, after));
    constexpr std::size_t image_width = 256, image_height = 256, image_pitch = 1056;
    std::vector<std::uint8_t> image(image_pitch * image_height, 0);
    for (std::size_t y = 0; y < image_height; ++y) {
        for (std::size_t x = 0; x < image_width; ++x) {
            auto* p = image.data() + y * image_pitch + x * 4;
            p[3] = 255;
            const int dy = static_cast<int>(y) - 26;
            const int dx = static_cast<int>(x) - 128;
            if (dy >= 0 && y <= 217 && (dx < 0 ? -dx : dx) * 192 <= dy * 102) p[0] = 255;
        }
    }
    assert(d3d12_probe::AnalyzeTriangle256(image.data(), image.size(), 0, image_pitch, after));
    std::vector<std::uint8_t> upside_down = image;
    for (std::size_t y = 0; y < image_height; ++y) {
        for (std::size_t x = 0; x < image_width * 4; ++x)
            upside_down[y * image_pitch + x] = image[(image_height - 1 - y) * image_pitch + x];
    }
    assert(!d3d12_probe::AnalyzeTriangle256(upside_down.data(), upside_down.size(), 0, image_pitch, after));
    for (std::size_t y = 0; y < image_height; ++y)
        for (std::size_t x = 0; x < image_width; ++x)
            image[y * image_pitch + x * 4] = (x >= 64 && x < 192 && y >= 64 && y < 192) ? 255 : 0;
    assert(!d3d12_probe::AnalyzeTriangle256(image.data(), image.size(), 0, image_pitch, after));
}
