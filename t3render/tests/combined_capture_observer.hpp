#pragma once

// Experimental observer shared by the synthetic fixture and the separately
// built opt-in proxy. The device boundary and runtime concurrency are not
// proven; an experimental capture is diagnostic and may be incomplete.
#include "prepared_buffer_capture.hpp"
#include "../src/frame_image.hpp"
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
#include "../src/scene_live_runtime.hpp"
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace t3render::fixture {

class CombinedCaptureObserver final {
public:
    enum class StateBlockAction { Unsupported, Create, Apply, Capture, Delete };
    struct ResetTicket { ir::ShaderTicket shader{}; std::uint64_t capture = 0, live = 0; };
    CombinedCaptureObserver(IDirect3DDevice8* native, buffers::BufferProxy& buffers,
                            const D3DPRESENT_PARAMETERS& presentation, bool fixture_force_history = false) noexcept
        : native_(native), buffers_(buffers), shaders_(ShaderLimits(fixture_force_history)), device_(shaders_.OpenDevice()) {
        if (writer_active_) { shaders_.MarkUnsafe(device_); deferred_abandon_ = true; }
        else { WriterScope scope; CaptureDevice(native_, &presentation); }
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
        if (writer_active_) live::RuntimeAbandon(); else live::RuntimeDeviceCreated(native_);
#endif
    }
    ~CombinedCaptureObserver() {
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
        live::RuntimeDeviceReleased(native_);
#endif
        shaders_.CloseDevice(device_);
        if (writer_active_) deferred_abandon_ = true;
        else { WriterScope scope; CaptureDeviceReleased(native_); }
    }
    bool valid() const noexcept { return native_ && device_; }
    void Invalidate() noexcept { shaders_.Invalidate(device_); }
    bool ShaderWindowAvailableForFixture() noexcept {
        if (reset_recovery_) return false;
        const auto ticket = shaders_.BeginDraw(device_);
        return ticket && shaders_.End(ticket);
    }
    ir::ShaderObservationStatus ShaderStatusForFixture(DWORD value) noexcept {
        if (reset_recovery_) return ir::ShaderObservationStatus::Unavailable;
        const auto ticket = shaders_.BeginDraw(device_);
        if (!ticket) return ir::ShaderObservationStatus::Unsafe;
        const auto status = shaders_.Observe(ticket, value).status;
        return shaders_.End(ticket) ? status : ir::ShaderObservationStatus::Unsafe;
    }
#ifndef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
    ir::ShaderObservation ShaderObservationForFixture(DWORD value) noexcept {
        if (reset_recovery_) return {};
        const auto ticket = shaders_.BeginDraw(device_);
        if (!ticket) return {};
        auto result = shaders_.Observe(ticket, value);
        return shaders_.End(ticket) ? result : ir::ShaderObservation{};
    }
#endif
    ResetTicket BeginReset() noexcept {
        const DWORD before = GetLastError();
        if (writer_active_) {
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
            live::RuntimeAbandon();
#endif
            Unsupported(); SetLastError(before); return {};
        }
        {
            std::lock_guard<std::mutex> lock(state_blocks_mutex_);
            state_blocks_.clear();
            state_blocks_enabled_ = false;
        }
        const auto ticket = shaders_.BeginReset(device_);
        std::uint64_t capture_ticket;
        { WriterScope scope; capture_ticket = CaptureResetBegin(native_, static_cast<std::uint32_t>(GetCurrentThreadId())); }
        std::uint64_t live_ticket=0;
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
        live_ticket=live::RuntimeResetBegin(native_);
#endif
        SetLastError(before);
        return {ticket, capture_ticket, live_ticket};
    }
    void EndReset(ResetTicket ticket, HRESULT hr, const D3DPRESENT_PARAMETERS* presentation) noexcept {
        const DWORD before = GetLastError();
        const bool stable = shaders_.EndReset(ticket.shader, static_cast<std::uint32_t>(hr));
        reset_recovery_ = FAILED(hr) || !stable;
        if (writer_active_) deferred_abandon_ = true;
        else { WriterScope scope; CaptureResetEnd(native_, ticket.capture, presentation, hr); }
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
        live::RuntimeResetEnd(ticket.live,hr);
#endif
        SetLastError(before);
    }
    void Unsupported(buffers::InvalidationCause cause = buffers::InvalidationCause::Explicit) noexcept {
        const DWORD before = GetLastError();
        if (writer_active_) {
            shaders_.MarkUnsafe(device_);
            deferred_abandon_ = true;
        } else {
            Invalidate();
            buffers_.InvalidateDevice(native_, cause);
#ifndef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
            WriterScope scope;
            CaptureAbandonDevice(native_);
#endif
        }
        SetLastError(before);
    }
    // Explicit fixture attestation: prior shader and state-block identities
    // were retired before this fully observed device epoch. Not a game API.
    bool EnableFixtureStateBlocks() noexcept {
        std::lock_guard<std::mutex> lock(state_blocks_mutex_);
        if (!valid() || reset_recovery_ || state_blocks_enabled_ || !state_blocks_.empty()) return false;
        state_blocks_enabled_ = true;
        return true;
    }
    ir::ShaderTicket BeginStateBlock(StateBlockAction action, DWORD token, D3DSTATEBLOCKTYPE type) noexcept {
        const DWORD before = GetLastError();
        if (writer_active_) { Unsupported(); SetLastError(before); return {}; }
        bool allowed;
        {
            std::lock_guard<std::mutex> lock(state_blocks_mutex_);
            allowed = state_blocks_enabled_ && !reset_recovery_ && action != StateBlockAction::Unsupported;
            if (action == StateBlockAction::Create) allowed = allowed && type == D3DSBT_ALL;
            else {
                const auto found = state_blocks_.find(token);
                allowed = allowed && found != state_blocks_.end() && found->second == shader_epoch_;
            }
        }
        const auto ticket = allowed ? shaders_.BeginSet(device_) : ir::ShaderTicket{};
        if (!ticket) Unsupported(buffers::InvalidationCause::StateBlock);
        SetLastError(before);
        return ticket;
    }
    void EndStateBlock(ir::ShaderTicket ticket, StateBlockAction action, DWORD token, DWORD* output, HRESULT hr) noexcept {
        const DWORD before = GetLastError();
        bool accepted = ticket && shaders_.EndSet(ticket, static_cast<std::uint32_t>(hr), 0) && SUCCEEDED(hr);
        if (accepted && action == StateBlockAction::Create) {
            std::uint32_t value = 0;
            accepted = ReadMemory(nullptr, reinterpret_cast<std::uintptr_t>(output), &value, sizeof(value)) && value;
            if (accepted) {
                try {
                    std::lock_guard<std::mutex> lock(state_blocks_mutex_);
                    accepted = state_blocks_.size() < 64 && state_blocks_.emplace(value, shader_epoch_).second;
                }
                catch (...) { accepted = false; }
            }
        } else if (accepted && action == StateBlockAction::Delete) {
            std::lock_guard<std::mutex> lock(state_blocks_mutex_);
            accepted = state_blocks_.erase(token) == 1;
        }
        if (!accepted) Unsupported(buffers::InvalidationCause::StateBlock);
        SetLastError(before);
    }

    HRESULT CreateVertexShader(const DWORD* declaration, const DWORD* program, DWORD* handle, DWORD usage) noexcept {
        const DWORD before = GetLastError();
        if (writer_active_) { Unsupported(); SetLastError(before); return native_->CreateVertexShader(declaration, program, handle, usage); }
        const auto ticket = shaders_.BeginCreate(device_);
        auto declaration_snapshot = ir::ScanDeclaration(reinterpret_cast<std::uintptr_t>(declaration), ReadMemory, nullptr);
        auto program_snapshot = ir::ScanVertexShader1x(reinterpret_cast<std::uintptr_t>(program), ReadMemory, nullptr, 1024);
        SetLastError(before);
        const HRESULT hr = native_->CreateVertexShader(declaration, program, handle, usage);
        const DWORD after = GetLastError();
        std::uint32_t value = 0;
        const bool known = SUCCEEDED(hr) && ReadMemory(nullptr, reinterpret_cast<std::uintptr_t>(handle), &value, sizeof(value));
        (void)shaders_.EndCreate(ticket, SUCCEEDED(hr), known, value, std::move(declaration_snapshot), std::move(program_snapshot));
        SetLastError(after);
        return hr;
    }
    HRESULT SetVertexShader(DWORD value) noexcept {
        const DWORD before = GetLastError();
        if (writer_active_) { Unsupported(); SetLastError(before); return native_->SetVertexShader(value); }
        const auto ticket = shaders_.BeginSet(device_);
        SetLastError(before);
        const HRESULT hr = native_->SetVertexShader(value);
        const DWORD after = GetLastError();
        (void)shaders_.EndSet(ticket, static_cast<std::uint32_t>(hr), value);
        SetLastError(after);
        return hr;
    }
    HRESULT DeleteVertexShader(DWORD value) noexcept {
        const DWORD before = GetLastError();
        if (writer_active_) { Unsupported(); SetLastError(before); return native_->DeleteVertexShader(value); }
        // A deleted shader can remain reachable through a block or the
        // current binding. Do not trust later handle reuse in this epoch.
        {
            std::lock_guard<std::mutex> lock(state_blocks_mutex_);
            ++shader_epoch_;
        }
        // Reset disables state-block tracking, but does not prove that a
        // deleted shader ceased to be bound or retained by an older block.
        // This hazard concerns shader identity; retain independent VB/IB
        // evidence so the resulting diagnostic frame can still be useful.
        Invalidate();
        const auto ticket = shaders_.BeginDelete(device_, value);
        SetLastError(before);
        const HRESULT hr = native_->DeleteVertexShader(value);
        const DWORD after = GetLastError();
        (void)shaders_.EndDelete(ticket, SUCCEEDED(hr));
        SetLastError(after);
        return hr;
    }
    HRESULT SetVertexShaderConstant(DWORD reg, const void* data, DWORD count) noexcept {
        const DWORD before = GetLastError();
        if (writer_active_) { Unsupported(); SetLastError(before); return native_->SetVertexShaderConstant(reg, data, count); }
        const auto ticket = shaders_.BeginSetConstants(device_);
        SetLastError(before);
        const HRESULT hr = native_->SetVertexShaderConstant(reg, data, count);
        const DWORD after = GetLastError();
        (void)shaders_.End(ticket);
        SetLastError(after);
        return hr;
    }
    HRESULT Present(const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty) noexcept {
        if (writer_active_) {
            const DWORD before = GetLastError();
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
            live::RuntimeDeferredLoss();
#endif
            Unsupported(); SetLastError(before);
            return native_->Present(source, dest, window, dirty);
        }
        const DWORD before = GetLastError();
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
        const auto live_ticket=live::RuntimePresentBegin(native_);
#endif
        std::uint64_t present_ticket;
        {
            WriterScope scope;
            present_ticket = CapturePresentBegin(native_, static_cast<std::uint32_t>(GetCurrentThreadId()));
            CaptureFrameImage(native_, ReadFrameImage, source != nullptr, dest != nullptr);
        }
        SetLastError(before);
        const HRESULT hr = native_->Present(source, dest, window, dirty);
        const DWORD after = GetLastError();
        {
            WriterScope scope;
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
            const auto* live_frame=live_ticket.ticket && live_ticket.frame_id!=UINT64_MAX ? &live_ticket.frame_id : nullptr;
            CapturePresentEnd(native_, present_ticket, hr, live_frame);
#else
            CapturePresentEnd(native_, present_ticket, hr);
#endif
        }
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
        live::RuntimePresentEnd(live_ticket,hr);
        // The hook leaves its pinned thunk/trampoline alive for in-flight
        // calls; disable it only after the capture lock has been released.
        static std::atomic<bool> disabled{false};
        if (!live::RuntimeEnabled() && CaptureBridgeShouldDisable() && !disabled.exchange(true)) {
            scene_bridge::ProbeStatus status{};
            (void)scene_bridge::DisableProbe(&status);
        }
#endif
        SetLastError(after);
        return hr;
    }
    HRESULT Clear(DWORD count, const D3DRECT* rectangles, DWORD flags, D3DCOLOR color, float depth, DWORD stencil) noexcept {
        const DWORD before = GetLastError();
        if (writer_active_) {
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
            live::RuntimeDeferredLoss();
#endif
            Unsupported(); SetLastError(before); return native_->Clear(count, rectangles, flags, color, depth, stencil);
        }
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
        DrawCall clear_call{};const auto live_draw=live::RuntimeDrawBegin(native_,clear_call,true);
#endif
        std::uint64_t operation;
        { WriterScope scope; operation = CaptureClear(native_, count, rectangles, flags, color, depth, stencil); }
        SetLastError(before);
        const HRESULT hr = native_->Clear(count, rectangles, flags, color, depth, stencil);
        const DWORD after = GetLastError();
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
        live::RuntimeDrawEnd(native_,live_draw,hr);
#endif
        { WriterScope scope; CaptureClearResult(native_, operation, hr); }
        SetLastError(after);
        return hr;
    }
    HRESULT DrawIndexed(D3DPRIMITIVETYPE type, UINT minimum, UINT vertices, UINT start, UINT count) noexcept {
        DrawCall call{};
        call.kind = DrawKind::IndexedPrimitive; call.primitive_type = type;
        call.min_vertex_index = minimum; call.num_vertices = vertices;
        call.start_index = start; call.primitive_count = count;
        return Draw(call, [&] { return native_->DrawIndexedPrimitive(type, minimum, vertices, start, count); });
    }
    HRESULT DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count) noexcept {
        DrawCall call{};
        call.kind = DrawKind::Primitive; call.primitive_type = type;
        call.start_vertex = start; call.primitive_count = count;
        return Draw(call, [&] { return native_->DrawPrimitive(type, start, count); });
    }
    HRESULT DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) noexcept {
        DrawCall call{};
        call.kind = DrawKind::PrimitiveUP; call.primitive_type = type; call.primitive_count = count;
        call.vertex_data = data; call.vertex_stride = stride;
        return Draw(call, [&] { return native_->DrawPrimitiveUP(type, count, data, stride); });
    }
    HRESULT DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT minimum, UINT vertices, UINT count,
                                   const void* indices, D3DFORMAT format, const void* data, UINT stride) noexcept {
        DrawCall call{};
        call.kind = DrawKind::IndexedPrimitiveUP; call.primitive_type = type; call.primitive_count = count;
        call.min_vertex_index = minimum; call.num_vertices = vertices; call.index_data = indices;
        call.index_format = format; call.vertex_data = data; call.vertex_stride = stride;
        return Draw(call, [&] { return native_->DrawIndexedPrimitiveUP(type, minimum, vertices, count, indices, format, data, stride); });
    }
private:
    static ir::ShaderRegistryLimits ShaderLimits(bool fixture_force_history) noexcept {
        const DWORD error = GetLastError();
        ir::ShaderRegistryLimits limits;
        if (fixture_force_history || CaptureDeclarationHistoryRequested()) {
            limits.preserve_reset_history = true;
            limits.words_per_device = 8192; // matches the advertised 32 KiB history limit
        }
        SetLastError(error);
        return limits;
    }
    template<class Forward>
    HRESULT Draw(DrawCall call, Forward forward) noexcept {
        const DWORD before = GetLastError();
        if (writer_active_) {
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
            live::RuntimeDeferredLoss();
#endif
            Unsupported(); SetLastError(before); return forward();
        }
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
        const auto live_draw=live::RuntimeDrawBegin(native_,call);
#endif
        const bool recovering = reset_recovery_;
#ifndef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
        if (recovering) {
            CaptureAbandonDevice(native_);
            SetLastError(before);
            return forward();
        }
#endif
        if (!CaptureWantsDraw(native_)) {
            SetLastError(before);
            const HRESULT hr=forward();
            const DWORD after=GetLastError();
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
            live::RuntimeDrawEnd(native_,live_draw,hr);
#endif
            SetLastError(after);return hr;
        }
        const auto buffer_ticket = buffers_.BeginDraw(native_);
        const auto shader_ticket = recovering ? ir::ShaderTicket{} : shaders_.BeginDraw(device_);
        ShaderSnapshot shader;
        if (!shader_ticket) shader.declaration.status = recovering ? ir::ShaderObservationStatus::Unavailable
                                                                    : ir::ShaderObservationStatus::Unsafe;
        shader.getter_result = native_->GetVertexShader(&shader.value);
        shader.constants_requested = true;
        shader.constants_count = CaptureVertexConstantCount(native_);
        if (shader_ticket && SUCCEEDED(shader.getter_result)) {
            shader.declaration = shaders_.Observe(shader_ticket, shader.value);
            if (shader.constants_count && shader.constants_count <= 96) {
                std::array<std::uint32_t, 96u * 4u + 16u> first, second;
                first.fill(0xa5a5a5a5u); second.fill(0x5a5a5a5au);
                shader.constants_result = native_->GetVertexShaderConstant(0, first.data(), shader.constants_count);
                shader.constants_verify_result = native_->GetVertexShaderConstant(0, second.data(), shader.constants_count);
                const auto words = static_cast<std::size_t>(shader.constants_count) * 4;
                shader.constants_match = SUCCEEDED(shader.constants_result) && SUCCEEDED(shader.constants_verify_result) &&
                    std::all_of(first.begin() + words, first.end(), [](auto v) { return v == 0xa5a5a5a5u; }) &&
                    std::all_of(second.begin() + words, second.end(), [](auto v) { return v == 0x5a5a5a5au; }) &&
                    std::equal(first.begin(), first.begin() + words, second.begin());
                if (shader.constants_match) std::copy_n(first.begin(), words, shader.constant_bits.begin());
            }
        }
        HRESULT hr;
        std::uint64_t operation;
        const bool buffered = call.kind == DrawKind::Primitive || call.kind == DrawKind::IndexedPrimitive;
        const bool indexed = call.kind == DrawKind::IndexedPrimitive;
        {
            PreparedBufferCapture prepared(buffers_, native_, buffered ? buffer_ticket : 0, indexed);
            call.shader_snapshot = &shader;
            if (buffered) prepared.Attach(call);
            { WriterScope scope; operation = CaptureDraw(native_, call); }
            SetLastError(before);
            hr = forward();
            const DWORD after = GetLastError();
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
            live::RuntimeDrawEnd(native_,live_draw,hr);
#endif
            const bool buffer_stable = buffer_ticket && buffers_.EndDraw(native_, buffer_ticket);
            const bool shader_stable = shader_ticket && shaders_.End(shader_ticket) && SUCCEEDED(hr) &&
                                       SUCCEEDED(shader.getter_result);
            { WriterScope scope; CaptureShaderDrawResult(native_, operation, hr, buffered ? (buffer_stable ? 1 : 0) : -1, shader_stable); }
            SetLastError(after);
        }
        return hr;
    }
    static bool ReadMemory(void*, std::uintptr_t address, void* output, std::size_t size) noexcept {
        SIZE_T copied = 0;
        return address && output && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                                                     output, size, &copied) && copied == size;
    }
    class WriterScope final {
    public:
        WriterScope() noexcept { writer_active_ = true; }
        ~WriterScope() {
            const DWORD before = GetLastError();
            writer_active_ = false;
            if (deferred_abandon_) {
                deferred_abandon_ = false;
                CaptureAbandonDevice(nullptr);
            }
            SetLastError(before);
        }
    };
    inline static thread_local bool writer_active_ = false;
    inline static thread_local bool deferred_abandon_ = false;
    IDirect3DDevice8* native_; // borrowed from fixture device
    buffers::BufferProxy& buffers_;
    ir::VertexDeclarationRegistry shaders_;
    std::uint64_t device_;
    std::atomic<bool> reset_recovery_{false};
    std::mutex state_blocks_mutex_;
    bool state_blocks_enabled_ = false;
    std::uint64_t shader_epoch_ = 1;
    std::unordered_map<DWORD, std::uint64_t> state_blocks_;
};

}  // namespace t3render::fixture
