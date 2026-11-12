#include "vertex_declaration_registry.hpp"

#include <limits>
#include <utility>

namespace t3render::ir {
namespace {
std::uint64_t Advance(std::uint64_t value) noexcept {
    return value == (std::numeric_limits<std::uint64_t>::max)() ? 0 : value + 1;
}
} // namespace

void VertexDeclarationRegistry::Poison(Device& device) noexcept {
    device.unsafe = true;
    device.pending = {};
    device.pending_handle = 0;
    device.handles.clear();
    device.words = 0;
}

std::uint64_t VertexDeclarationRegistry::OpenDevice() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!next_device_ || devices_.size() >= limits_.devices) return 0;
        const auto id = next_device_;
        devices_.emplace(id, Device{});
        next_device_ = Advance(id);
        return id;
    } catch (...) { return 0; }
}
void VertexDeclarationRegistry::CloseDevice(std::uint64_t device) noexcept {
    try { std::lock_guard<std::mutex> lock(mutex_); devices_.erase(device); } catch (...) {}
}
void VertexDeclarationRegistry::Invalidate(std::uint64_t device) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = devices_.find(device);
        if (it != devices_.end()) Poison(it->second);
    } catch (...) {}
}
bool VertexDeclarationRegistry::Matches(const Device& device, ShaderTicket ticket) const noexcept {
    return !device.unsafe && ticket && device.pending.device == ticket.device &&
           device.pending.sequence == ticket.sequence && device.pending.kind == ticket.kind;
}

ShaderTicket VertexDeclarationRegistry::Begin(std::uint64_t id, ShaderOperation kind, std::uint32_t handle) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = devices_.find(id);
        if (it == devices_.end() || it->second.unsafe) return {};
        auto& device = it->second;
        if (device.pending || !device.next_operation || kind == ShaderOperation::None) {
            Poison(device); return {};
        }
        const auto sequence = device.next_operation;
        device.next_operation = Advance(sequence);
        device.pending = {id, sequence, kind};
        if (kind == ShaderOperation::Delete) {
            device.pending_handle = handle;
            const auto entry = device.handles.find(handle);
            if (entry != device.handles.end()) {
                device.words -= entry->second.words.capacity();
                std::vector<std::uint32_t>().swap(entry->second.words);
                entry->second.available = false;
            }
        } else if (kind == ShaderOperation::Reset) {
            // Shader survival across Reset has not been verified. Retain
            // tombstones so a returned numeric handle cannot silently replace
            // a formerly observed shader on this device.
            for (auto& item : device.handles) {
                std::vector<std::uint32_t>().swap(item.second.words);
                item.second.available = false;
            }
            device.words = 0;
        }
        return device.pending;
    } catch (...) { Invalidate(id); return {}; }
}
ShaderTicket VertexDeclarationRegistry::BeginCreate(std::uint64_t id) noexcept { return Begin(id, ShaderOperation::Create, 0); }
ShaderTicket VertexDeclarationRegistry::BeginDelete(std::uint64_t id, std::uint32_t handle) noexcept { return Begin(id, ShaderOperation::Delete, handle); }
ShaderTicket VertexDeclarationRegistry::BeginSet(std::uint64_t id) noexcept { return Begin(id, ShaderOperation::Set, 0); }
ShaderTicket VertexDeclarationRegistry::BeginReset(std::uint64_t id) noexcept { return Begin(id, ShaderOperation::Reset, 0); }
ShaderTicket VertexDeclarationRegistry::BeginDraw(std::uint64_t id) noexcept { return Begin(id, ShaderOperation::Draw, 0); }

bool VertexDeclarationRegistry::EndCreate(ShaderTicket ticket, bool native_success, bool handle_known,
                                          std::uint32_t handle, DeclarationSnapshot snapshot) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = devices_.find(ticket.device);
        if (it == devices_.end()) return false;
        auto& device = it->second;
        if (ticket.kind != ShaderOperation::Create || !Matches(device, ticket)) {
            if (!device.unsafe) Poison(device);
            return false;
        }
        if (!native_success) { device.pending = {}; return true; }
        if (!handle_known || !device.next_generation) { Poison(device); return false; }
        const auto old = device.handles.find(handle);
        if (old != device.handles.end()) {
            // A successful Create returning a still-observed numeric handle
            // is ambiguous. Do not assume the older object was replaced.
            device.words -= old->second.words.capacity();
            std::vector<std::uint32_t>().swap(old->second.words);
            old->second.available = false;
            old->second.generation = device.next_generation;
            device.next_generation = Advance(device.next_generation);
            device.pending = {};
            return true;
        }
        const bool complete = snapshot.status == DeclarationStatus::Complete && !snapshot.words.empty() &&
                              snapshot.words.back() == 0xffffffffu;
        const std::size_t size = complete ? snapshot.words.size() : 0;
        if (device.handles.size() >= limits_.handles_per_device || size > limits_.words_per_declaration ||
            size > limits_.words_per_device || device.words > limits_.words_per_device - size) {
            Poison(device); return false;
        }
        try {
            Entry entry;
            entry.generation = device.next_generation;
            entry.available = complete;
            // Copy into owned bounded storage: a caller-provided vector may
            // have much more capacity than the declaration's word count.
            if (complete) entry.words.assign(snapshot.words.begin(), snapshot.words.end());
            const std::size_t capacity = entry.words.capacity();
            if (capacity > limits_.words_per_device || device.words > limits_.words_per_device - capacity) {
                Poison(device); return false;
            }
            device.handles.emplace(handle, std::move(entry));
            device.words += capacity;
            device.next_generation = Advance(device.next_generation);
            device.pending = {};
            return true;
        } catch (...) { Poison(device); return false; }
    } catch (...) { Invalidate(ticket.device); return false; }
}

bool VertexDeclarationRegistry::EndDelete(ShaderTicket ticket, bool native_success) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = devices_.find(ticket.device);
        if (it == devices_.end()) return false;
        auto& device = it->second;
        if (ticket.kind != ShaderOperation::Delete || !Matches(device, ticket)) {
            if (!device.unsafe) Poison(device);
            return false;
        }
        if (native_success) device.handles.erase(device.pending_handle);
        device.pending = {};
        device.pending_handle = 0;
        return true;
    } catch (...) { Invalidate(ticket.device); return false; }
}

bool VertexDeclarationRegistry::End(ShaderTicket ticket) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = devices_.find(ticket.device);
        if (it == devices_.end()) return false;
        auto& device = it->second;
        if (ticket.kind == ShaderOperation::Create || ticket.kind == ShaderOperation::Delete || !Matches(device, ticket)) {
            if (!device.unsafe) Poison(device);
            return false;
        }
        device.pending = {};
        return true;
    } catch (...) { Invalidate(ticket.device); return false; }
}

ShaderObservation VertexDeclarationRegistry::Observe(ShaderTicket ticket, std::uint32_t value) noexcept {
    ShaderObservation result;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = devices_.find(ticket.device);
        if (it == devices_.end() || ticket.kind != ShaderOperation::Draw || !Matches(it->second, ticket)) {
            result.status = ShaderObservationStatus::Unsafe; return result;
        }
        const auto entry = it->second.handles.find(value);
        if (entry == it->second.handles.end()) return result; // FVF or unobserved handle
        result.generation = entry->second.generation;
        if (!entry->second.available) { result.status = ShaderObservationStatus::Unavailable; return result; }
        result.words = entry->second.words;
        result.status = ShaderObservationStatus::Candidate;
    } catch (...) { result = {}; result.status = ShaderObservationStatus::Unavailable; }
    return result;
}

} // namespace t3render::ir
