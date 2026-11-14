#pragma once

// Fixture-only adapter: acquire native getter references and canonicalize
// before entering the capture writer. The owner and draw ticket must remain
// live until after CaptureDraw and the corresponding native draw.
#include "../src/buffer_proxy.hpp"
#include "../src/capture.hpp"

namespace t3render::fixture {

class PreparedBufferCapture final {
public:
    PreparedBufferCapture(buffers::BufferProxy& manager, IDirect3DDevice8* device,
                          std::uint64_t draw, bool indexed) noexcept {
        const DWORD error = GetLastError();
        bindings_.device = device;
        observer_ = {this, draw != 0, ReadVertex, ReadIndex};
        if (device && !draw) {
            const auto failure = manager.DescribeDrawFailure(device);
            observer_.failure_status = failure.status;
            observer_.failure_cause = failure.cause;
            observer_.failure_line = failure.source_line;
            observer_.failure_unlock = failure.unlock;
        }
        if (device && draw) {
            bindings_.vertex_result = device->GetStreamSource(0, &bindings_.vertex, &bindings_.stride);
            if (SUCCEEDED(bindings_.vertex_result) && bindings_.vertex)
                vertex_ = manager.PrepareVertexGetter(device, bindings_.vertex, draw);
            if (indexed) {
                bindings_.index_result = device->GetIndices(&bindings_.index, &bindings_.base);
                if (SUCCEEDED(bindings_.index_result) && bindings_.index)
                    index_ = manager.PrepareIndexGetter(device, bindings_.index, draw);
            }
        }
        SetLastError(error);
    }
    ~PreparedBufferCapture() {
        const DWORD error = GetLastError();
        if (bindings_.vertex) bindings_.vertex->Release();
        if (bindings_.index) bindings_.index->Release();
        SetLastError(error);
    }
    PreparedBufferCapture(const PreparedBufferCapture&) = delete;
    PreparedBufferCapture& operator=(const PreparedBufferCapture&) = delete;

    void Attach(DrawCall& call) const noexcept {
        call.buffer_observer = &observer_;
        call.buffer_bindings = &bindings_;
    }
private:
    static bool ReadVertex(void* context, IDirect3DDevice8* device, IDirect3DVertexBuffer8* buffer,
                           std::size_t offset, void* destination, std::size_t length) noexcept {
        const auto& self = *static_cast<PreparedBufferCapture*>(context);
        return self.vertex_.Read(device, buffer, offset, destination, length) == ir::RegistryResult::Ok;
    }
    static bool ReadIndex(void* context, IDirect3DDevice8* device, IDirect3DIndexBuffer8* buffer,
                          std::size_t offset, void* destination, std::size_t length) noexcept {
        const auto& self = *static_cast<PreparedBufferCapture*>(context);
        return self.index_.Read(device, buffer, offset, destination, length) == ir::RegistryResult::Ok;
    }
    BufferBindingSnapshot bindings_{};
    BufferObserver observer_{};
    buffers::BufferProxy::PreparedVertex vertex_;
    buffers::BufferProxy::PreparedIndex index_;
};

}  // namespace t3render::fixture
