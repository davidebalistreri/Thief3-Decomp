#pragma once

#include "capture.hpp"
#include "scene_live_driver.hpp"

namespace t3render::live {
// Opt-in runtime adapter. Initialization runs outside DllMain and only reads
// the local catalog. Hook failures or unsafe device ambiguity disable output.
bool RuntimeInitialize() noexcept;
bool RuntimeEnabled() noexcept;
// Immutable, process-local random session ID; null when live setup failed.
const char* RuntimeSessionId() noexcept;
void RuntimeAbandon() noexcept;
void RuntimeDeferredLoss() noexcept; // lock-free, safe from nested probe callbacks

void RuntimeDeviceCreated(IDirect3DDevice8* device) noexcept;
void RuntimeDeviceReleased(IDirect3DDevice8* device) noexcept;
std::uint64_t RuntimeProbeBegin(std::uint32_t thread_id) noexcept;
void RuntimeProbeEnd(std::uint64_t ticket, const scene_bridge::ProbeSnapshot& snapshot) noexcept;
void RuntimeSubmissionEnter(std::uint32_t element, std::uint32_t stage, std::uint32_t section) noexcept;
void RuntimeSubmissionLeave() noexcept;

struct RuntimeDraw {
    std::uint64_t ticket=0;
    bool needs_state=false;
    DrawObservation before;
};
RuntimeDraw RuntimeDrawBegin(IDirect3DDevice8* device, const DrawCall& call) noexcept;
void RuntimeDrawEnd(IDirect3DDevice8* device, const RuntimeDraw& draw, HRESULT result) noexcept;
struct RuntimePresent { std::uint64_t ticket=0, frame_id=UINT64_MAX; };
RuntimePresent RuntimePresentBegin(IDirect3DDevice8* device) noexcept;
void RuntimePresentEnd(RuntimePresent present, HRESULT result) noexcept;
std::uint64_t RuntimeResetBegin(IDirect3DDevice8* device) noexcept;
void RuntimeResetEnd(std::uint64_t ticket, HRESULT result) noexcept;
std::shared_ptr<const Snapshot> RuntimeCurrent() noexcept;
} // namespace t3render::live
