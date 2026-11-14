#pragma once

// Isolated single-threaded fixture observer. This is not a complete device
// boundary and must not be installed in TDS.
#include "prepared_buffer_capture.hpp"
#include <algorithm>
#include <array>

namespace t3render::fixture {

class CombinedCaptureObserver final {
public:
    CombinedCaptureObserver(IDirect3DDevice8* native, buffers::BufferProxy& buffers,
                            const D3DPRESENT_PARAMETERS& presentation) noexcept
        : native_(native), buffers_(buffers), device_(shaders_.OpenDevice()) {
        CaptureDevice(native_, &presentation);
    }
    ~CombinedCaptureObserver() {
        shaders_.CloseDevice(device_);
        CaptureDeviceReleased(native_);
    }
    bool valid() const noexcept { return native_ && device_; }
    void Invalidate() noexcept { shaders_.Invalidate(device_); }
    bool ShaderWindowAvailableForFixture() noexcept {
        const auto ticket = shaders_.BeginDraw(device_);
        return ticket && shaders_.End(ticket);
    }
    void Unsupported() noexcept {
        const DWORD before = GetLastError();
        Invalidate();
        buffers_.InvalidateDevice(native_);
        CaptureAbandonDevice(native_);
        SetLastError(before);
    }

    HRESULT CreateVertexShader(const DWORD* declaration, const DWORD* program, DWORD* handle, DWORD usage) noexcept {
        const DWORD before = GetLastError();
        const auto ticket = shaders_.BeginCreate(device_);
        auto declaration_snapshot = ir::ScanDeclaration(reinterpret_cast<std::uintptr_t>(declaration), ReadMemory, nullptr);
        auto program_snapshot = ir::ScanVertexShader11(reinterpret_cast<std::uintptr_t>(program), ReadMemory, nullptr, 1024);
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
        const auto ticket = shaders_.BeginSetConstants(device_);
        SetLastError(before);
        const HRESULT hr = native_->SetVertexShaderConstant(reg, data, count);
        const DWORD after = GetLastError();
        (void)shaders_.End(ticket);
        SetLastError(after);
        return hr;
    }
    HRESULT Present(const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty) noexcept {
        const HRESULT hr = native_->Present(source, dest, window, dirty);
        const DWORD after = GetLastError();
        CapturePresent(native_, hr);
        SetLastError(after);
        return hr;
    }
    HRESULT Clear(DWORD count, const D3DRECT* rectangles, DWORD flags, D3DCOLOR color, float depth, DWORD stencil) noexcept {
        const DWORD before = GetLastError();
        const auto operation = CaptureClear(native_, count, rectangles, flags, color, depth, stencil);
        SetLastError(before);
        const HRESULT hr = native_->Clear(count, rectangles, flags, color, depth, stencil);
        const DWORD after = GetLastError();
        CaptureClearResult(native_, operation, hr);
        SetLastError(after);
        return hr;
    }
    HRESULT DrawIndexed(D3DPRIMITIVETYPE type, UINT minimum, UINT vertices, UINT start, UINT count) noexcept {
        const DWORD before = GetLastError();
        if (!CaptureWantsDraw(native_)) {
            SetLastError(before);
            return native_->DrawIndexedPrimitive(type, minimum, vertices, start, count);
        }
        const auto buffer_ticket = buffers_.BeginDraw(native_);
        const auto shader_ticket = shaders_.BeginDraw(device_);
        ShaderSnapshot shader;
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
        bool buffer_stable;
        {
            PreparedBufferCapture prepared(buffers_, native_, buffer_ticket, true);
            DrawCall call{};
            call.kind = DrawKind::IndexedPrimitive; call.primitive_type = type;
            call.min_vertex_index = minimum; call.num_vertices = vertices;
            call.start_index = start; call.primitive_count = count; call.shader_snapshot = &shader;
            prepared.Attach(call);
            operation = CaptureDraw(native_, call);
            SetLastError(before);
            hr = native_->DrawIndexedPrimitive(type, minimum, vertices, start, count);
            const DWORD after = GetLastError();
            buffer_stable = buffer_ticket && buffers_.EndDraw(native_, buffer_ticket);
            const bool shader_stable = shader_ticket && shaders_.End(shader_ticket) && SUCCEEDED(hr) &&
                                       SUCCEEDED(shader.getter_result);
            CaptureShaderDrawResult(native_, operation, hr, buffer_stable ? 1 : 0, shader_stable);
            SetLastError(after);
        }
        return hr;
    }
private:
    static bool ReadMemory(void*, std::uintptr_t address, void* output, std::size_t size) noexcept {
        SIZE_T copied = 0;
        return address && output && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                                                     output, size, &copied) && copied == size;
    }
    IDirect3DDevice8* native_; // borrowed from fixture device
    buffers::BufferProxy& buffers_;
    ir::VertexDeclarationRegistry shaders_;
    std::uint64_t device_;
};

}  // namespace t3render::fixture
