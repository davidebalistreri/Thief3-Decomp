#pragma once

#include "../ir/scene_live_window.hpp"

#include <array>
#include <cstdint>
#include <limits>

namespace t3render::live {
// Bounds for a cumulative histogram of consecutive D3D8 Present entry
// intervals. The final bucket contains intervals above the last bound.
inline constexpr std::array<unsigned, 19> kPresentTimingBoundsMs{
    1,2,4,6,8,10,12,14,16,18,20,25,33,50,75,100,250,500,1000};
struct PresentClockSample { std::uint64_t ticks=0;bool available=false; };
struct PresentTimingStats {
    std::uint64_t clock_hz=0,successful=0,failed=0,overlaps=0,resets=0,interruptions=0,clock_errors=0;
    std::uint64_t intervals=0,total_ticks=0,max_ticks=0;
    std::array<std::uint64_t,kPresentTimingBoundsMs.size()+1> histogram{};
};
struct PresentTimingMark {
    ThreadToken thread{};
    std::uint64_t ticks=0,generation=0;
    bool valid=false;
};

// Called only while the driver's existing mutex is held. No I/O or allocation
// occurs on the game's thread. A mark survives until that Present ends.
class PresentTiming {
public:
    explicit PresentTiming(std::uint64_t frequency=0) noexcept { stats_.clock_hz=frequency; }
    PresentTimingMark Begin(ThreadToken thread,PresentClockSample sample,bool overlap,bool resetting) noexcept {
        if (!stats_.clock_hz) return {};
        if (overlap) {++stats_.overlaps;Break();}
        if (resetting) Break();
        if (!sample.available) {++stats_.clock_errors;Break();}
        if (!thread.id || !thread.birth) Break();
        return {thread,sample.ticks,generation_,sample.available && thread.id && thread.birth && !overlap && !resetting};
    }
    void End(const PresentTimingMark& mark,ThreadToken thread,bool succeeded) noexcept {
        if (!stats_.clock_hz) return;
        if (succeeded) ++stats_.successful; else ++stats_.failed;
        if (!succeeded || !SameThread(mark.thread,thread)) {Break();return;}
        if (!mark.valid || mark.generation!=generation_) return;
        if (has_previous_) {
            if (!SameThread(previous_thread_,thread)) Break();
            else if (mark.ticks<=previous_ticks_) {++stats_.clock_errors;Break();}
            else {
                const auto delta=mark.ticks-previous_ticks_;
                if (delta>std::numeric_limits<std::uint64_t>::max()-stats_.total_ticks) {
                    ++stats_.clock_errors;Break();
                } else {
                    ++stats_.intervals;stats_.total_ticks+=delta;
                    if (delta>stats_.max_ticks) stats_.max_ticks=delta;
                    const double ms=static_cast<double>(delta)*1000.0/static_cast<double>(stats_.clock_hz);
                    std::size_t bucket=0;
                    while (bucket<kPresentTimingBoundsMs.size() && ms>kPresentTimingBoundsMs[bucket]) ++bucket;
                    ++stats_.histogram[bucket];
                }
            }
        }
        previous_ticks_=mark.ticks;previous_thread_=thread;has_previous_=true;
    }
    void Reset() noexcept {if (stats_.clock_hz) {++stats_.resets;Break();}}
    void Interrupt() noexcept {if (stats_.clock_hz) Break();}
    const PresentTimingStats& stats() const noexcept {return stats_;}
private:
    static bool SameThread(ThreadToken a,ThreadToken b) noexcept {
        return a.id && a.birth && a.id==b.id && a.birth==b.birth;
    }
    void Break() noexcept {has_previous_=false;++generation_;++stats_.interruptions;}
    PresentTimingStats stats_{};
    ThreadToken previous_thread_{};
    std::uint64_t previous_ticks_=0,generation_=1;
    bool has_previous_=false;
};
} // namespace t3render::live
