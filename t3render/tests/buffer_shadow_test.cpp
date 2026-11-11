#include "../ir/buffer_shadow.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

using t3render::ir::BufferShadow;
using t3render::ir::ShadowResult;
using t3render::ir::ShadowStatus;

static_assert(!std::is_copy_constructible_v<BufferShadow>);
static_assert(!std::is_move_constructible_v<BufferShadow>);

void ConstructionAndBounds() {
    BufferShadow zero(0, 0);
    assert(zero.status() == ShadowStatus::Ready && zero.storage_bytes() == 0);
    assert(zero.Write(0, nullptr, 0) == ShadowResult::Ok);
    assert(zero.Read(0, nullptr, 0) == ShadowResult::Ok);
    assert(zero.Invalidate(0, 0) == ShadowResult::Ok);
    assert(zero.Discard() == ShadowResult::Ok);
    bool known = false;
    assert(zero.IsKnown(0, 0, known) == ShadowResult::Ok && known);
    assert(zero.Write(1, nullptr, 0) == ShadowResult::OutOfBounds);

    BufferShadow too_little(9, 10);  // Nine data bytes and two validity bytes.
    assert(too_little.status() == ShadowStatus::BudgetExceeded);
    assert(too_little.size() == 9 && too_little.budget() == 10 && too_little.storage_bytes() == 0);
    assert(too_little.Write(0, nullptr, 0) == ShadowResult::Unavailable);
    assert(too_little.Discard() == ShadowResult::Unavailable);
    known = true;
    assert(too_little.IsKnown(0, 0, known) == ShadowResult::Unavailable && known);

    BufferShadow one_short(1, 1);
    BufferShadow one_exact(1, 2);
    assert(one_short.status() == ShadowStatus::BudgetExceeded);
    assert(one_exact.status() == ShadowStatus::Ready && one_exact.storage_bytes() == 2);
    BufferShadow exact(9, 11);
    assert(exact.status() == ShadowStatus::Ready && exact.storage_bytes() == 11);
    assert(exact.Write(9, nullptr, 0) == ShadowResult::Ok);
    assert(exact.Write(10, nullptr, 0) == ShadowResult::OutOfBounds);
    assert(exact.Read(9, nullptr, 0) == ShadowResult::Ok);
    assert(exact.Read(8, nullptr, 1) == ShadowResult::NullPointer);
    const auto maximum = std::numeric_limits<std::size_t>::max();
    assert(exact.Write(maximum, nullptr, 1) == ShadowResult::OutOfBounds);
    assert(exact.Invalidate(1, maximum) == ShadowResult::OutOfBounds);
    assert(exact.Read(8, nullptr, maximum) == ShadowResult::OutOfBounds);
    BufferShadow impossible(maximum, maximum);
    assert(impossible.status() == ShadowStatus::TooLarge && impossible.storage_bytes() == 0);
}

void Intervals() {
    BufferShadow shadow(19, 22);  // 19 data bytes, 3 validity bytes.
    assert(shadow.status() == ShadowStatus::Ready);
    std::array<std::uint8_t, 19> source{};
    for (std::size_t i = 0; i < source.size(); ++i) source[i] = static_cast<std::uint8_t>(i + 1);
    std::array<std::uint8_t, 19> output{};
    output.fill(0xee);
    assert(shadow.Read(0, output.data(), output.size()) == ShadowResult::UnknownData);
    assert(std::all_of(output.begin(), output.end(), [](auto b) { return b == 0xee; }));

    assert(shadow.Write(0, source.data(), 7) == ShadowResult::Ok);
    assert(shadow.Write(7, source.data() + 7, 5) == ShadowResult::Ok);
    assert(shadow.Write(12, source.data() + 12, 7) == ShadowResult::Ok);
    assert(shadow.Read(0, output.data(), output.size()) == ShadowResult::Ok && output == source);
    const std::array<std::uint8_t, 6> replacement{{88, 89, 90, 91, 92, 93}};
    assert(shadow.Write(5, replacement.data(), replacement.size()) == ShadowResult::Ok);
    std::copy(replacement.begin(), replacement.end(), source.begin() + 5);
    assert(shadow.Read(0, output.data(), output.size()) == ShadowResult::Ok && output == source);

    assert(shadow.Invalidate(6, 5) == ShadowResult::Ok);  // Crosses a bitset boundary.
    bool known = true;
    assert(shadow.IsKnown(0, 6, known) == ShadowResult::Ok && known);
    assert(shadow.IsKnown(6, 1, known) == ShadowResult::Ok && !known);
    assert(shadow.IsKnown(11, 8, known) == ShadowResult::Ok && known);
    output.fill(0xee);
    assert(shadow.Read(0, output.data(), output.size()) == ShadowResult::UnknownData);
    assert(std::all_of(output.begin(), output.end(), [](auto b) { return b == 0xee; }));
    assert(shadow.Write(6, source.data() + 6, 3) == ShadowResult::Ok);
    assert(shadow.IsKnown(0, 9, known) == ShadowResult::Ok && known);
    assert(shadow.IsKnown(9, 2, known) == ShadowResult::Ok && !known);
    assert(shadow.Write(9, source.data() + 9, 2) == ShadowResult::Ok);
    assert(shadow.Read(0, output.data(), output.size()) == ShadowResult::Ok && output == source);

    assert(shadow.Write(0, nullptr, 1) == ShadowResult::NullPointer);
    assert(shadow.Invalidate(17, 3) == ShadowResult::OutOfBounds);
    assert(shadow.Write(18, source.data(), 2) == ShadowResult::OutOfBounds);
    known = true;
    assert(shadow.IsKnown(20, 0, known) == ShadowResult::OutOfBounds && known);
    assert(shadow.Read(0, output.data(), output.size()) == ShadowResult::Ok && output == source);
    assert(shadow.Discard() == ShadowResult::Ok && shadow.storage_bytes() == 22);
    assert(shadow.IsKnown(0, 19, known) == ShadowResult::Ok && !known);
    assert(shadow.Write(8, source.data() + 8, 2) == ShadowResult::Ok);
    assert(shadow.IsKnown(8, 2, known) == ShadowResult::Ok && known);
    assert(shadow.IsKnown(7, 1, known) == ShadowResult::Ok && !known);
    assert(shadow.IsKnown(10, 1, known) == ShadowResult::Ok && !known);
    assert(shadow.Discard() == ShadowResult::Ok);
    assert(shadow.IsKnown(8, 2, known) == ShadowResult::Ok && !known);
}

std::uint32_t Next(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

void ReferenceModel() {
    constexpr std::size_t size = 57;
    BufferShadow shadow(size, size + 8);
    assert(shadow.status() == ShadowStatus::Ready);
    std::array<std::uint8_t, size> expected{};
    std::array<bool, size> known{};
    std::array<std::uint8_t, size> payload{};
    std::array<std::uint8_t, size> output{};
    std::uint32_t random = 0x39a1e5u;
    for (int iteration = 0; iteration < 5000; ++iteration) {
        const std::size_t offset = Next(random) % (size + 1);
        const std::size_t length = Next(random) % (size - offset + 1);
        switch (Next(random) % 4) {
            case 0:
            case 1:
                for (std::size_t i = 0; i < length; ++i) payload[i] = static_cast<std::uint8_t>(Next(random));
                assert(shadow.Write(offset, payload.data(), length) == ShadowResult::Ok);
                for (std::size_t i = 0; i < length; ++i) { expected[offset + i] = payload[i]; known[offset + i] = true; }
                break;
            case 2:
                assert(shadow.Invalidate(offset, length) == ShadowResult::Ok);
                for (std::size_t i = 0; i < length; ++i) known[offset + i] = false;
                break;
            default:
                assert(shadow.Discard() == ShadowResult::Ok);
                known.fill(false);
                break;
        }
        const std::size_t read_offset = Next(random) % (size + 1);
        const std::size_t read_length = Next(random) % (size - read_offset + 1);
        bool expected_known = true;
        for (std::size_t i = 0; i < read_length; ++i) expected_known = expected_known && known[read_offset + i];
        bool actual_known = !expected_known;
        assert(shadow.IsKnown(read_offset, read_length, actual_known) == ShadowResult::Ok && actual_known == expected_known);
        output.fill(0xcc);
        const ShadowResult result = shadow.Read(read_offset, output.data(), read_length);
        assert(result == (expected_known ? ShadowResult::Ok : ShadowResult::UnknownData));
        for (std::size_t i = 0; i < size; ++i) {
            const auto want = expected_known && i < read_length ? expected[read_offset + i] : 0xcc;
            assert(output[i] == want);
        }
    }
}

int main() {
    ConstructionAndBounds();
    Intervals();
    ReferenceModel();
}
