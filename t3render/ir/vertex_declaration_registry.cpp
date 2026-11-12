#include "vertex_declaration_registry.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace t3render::ir {
namespace {
std::uint64_t Advance(std::uint64_t value) noexcept {
    return value == (std::numeric_limits<std::uint64_t>::max)() ? 0 : value + 1;
}
// Bound the metadata preallocated before any registry lock is taken.
constexpr std::size_t kMaxDevices = 64;
constexpr std::size_t kMaxHandlesPerDevice = 4096;
constexpr std::size_t kMaxSlots = 65536;
struct DrainScope {
    const VertexDeclarationRegistry* registry;
    DrainScope* previous;
};
thread_local DrainScope* active_drain = nullptr;
struct ScopedDrain {
    DrainScope scope;
    explicit ScopedDrain(const VertexDeclarationRegistry* registry) noexcept : scope{registry, active_drain} {
        active_drain = &scope;
    }
    ~ScopedDrain() { active_drain = scope.previous; }
};
} // namespace

VertexDeclarationRegistry::VertexDeclarationRegistry(ShaderRegistryLimits limits) noexcept : limits_(limits) {
    if (!limits_.devices || !limits_.handles_per_device || limits_.devices > kMaxDevices ||
        limits_.handles_per_device > kMaxHandlesPerDevice ||
        limits_.devices > kMaxSlots / limits_.handles_per_device) return;
    try {
        std::vector<Device> devices(limits_.devices);
        for (auto& device : devices) device.handles.resize(limits_.handles_per_device);
        devices_ = std::move(devices);
        for (auto& device : devices_) for (auto& entry : device.handles) entry.owner = &device;
    } catch (...) {
        // No device can be opened if initial bounded metadata allocation fails.
    }
}

VertexDeclarationRegistry::Device* VertexDeclarationRegistry::FindLocked(std::uint64_t id, bool include_closing) noexcept {
    for (auto& device : devices_)
        if (device.id == id && (device.active || (include_closing && device.closing))) return &device;
    return nullptr;
}
VertexDeclarationRegistry::Entry* VertexDeclarationRegistry::FindHandleLocked(Device& device, std::uint32_t handle) noexcept {
    for (auto& entry : device.handles)
        if (entry.occupied && entry.handle == handle) return &entry;
    return nullptr;
}
VertexDeclarationRegistry::Entry* VertexDeclarationRegistry::FreeHandleLocked(Device& device) noexcept {
    for (auto& entry : device.handles)
        if (!entry.occupied && !entry.payload && !entry.queued) return &entry;
    return nullptr;
}
void VertexDeclarationRegistry::EnqueueLocked(Entry& entry) noexcept {
    if (!entry.payload || entry.queued) return;
    entry.queued = true;
    entry.retired_next = nullptr;
    if (retired_tail_) retired_tail_->retired_next = &entry;
    else retired_head_ = &entry;
    retired_tail_ = &entry;
    ++retired_count_;
    ++retire_epoch_;
}
void VertexDeclarationRegistry::PoisonLocked(Device& device) noexcept {
    device.unsafe = true;
    device.pending = {};
    device.pending_handle = 0;
    device.rebind_epoch = 0;
    for (auto& entry : device.handles) {
        entry.available = false;
        entry.reset_history = false;
        entry.reset_epoch = 0;
        EnqueueLocked(entry);
    }
}

void VertexDeclarationRegistry::FinishClosingLocked(Device& device) noexcept {
    if (!device.closing || device.preparing || device.retiring || device.words) return;
    for (const auto& entry : device.handles) if (entry.payload || entry.queued) return;
    for (auto& entry : device.handles) {
        entry.occupied = false;
        entry.available = false;
        entry.handle = 0;
        entry.generation = 0;
        entry.reason = ShaderUnavailableReason::None;
        entry.scan_status = DeclarationStatus::Unreadable;
        entry.reset_history = false;
        entry.reset_epoch = 0;
        entry.reset_hr = 0;
    }
    device.id = 0;
    device.closing = false;
}

void VertexDeclarationRegistry::FinishPreparation(std::uint64_t id, std::size_t words) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        Device* device = FindLocked(id, true);
        if (!device || !device->preparing || device->words < words) return;
        device->words -= words;
        --device->preparing;
        FinishClosingLocked(*device);
    } catch (...) {}
}

// Move one unreachable payload at a time out of the table and drop its last
// reference only after unlocking. Reentrant drains of this registry defer to
// the outer sweep, preventing recursive allocator callbacks from exhausting
// the stack. The sweep also completes any reentrant CloseDevice calls.
void VertexDeclarationRegistry::Drain() noexcept {
    for (DrainScope* scope = active_drain; scope; scope = scope->previous)
        if (scope->registry == this) return;
    ScopedDrain active(this);
    // Keep a maliciously reentrant allocator or continuously changing set of
    // pins from trapping one call forever. Remaining work stays queued; later
    // unpins or registry operations retry it without relaxing memory quotas.
    std::size_t allowance = devices_.size() * limits_.handles_per_device * 2 + 1;
    for (;;) {
        std::size_t remaining = 0;
        std::uint64_t epoch = 0;
        try {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                remaining = retired_count_;
                epoch = retire_epoch_;
            }
            while (remaining > 0 && allowance > 0) {
                --remaining;
                --allowance;
                std::shared_ptr<const Payload> retired;
                Device* owner = nullptr;
                std::size_t words = 0;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    Entry* entry = retired_head_;
                    if (!entry) break;
                    if (entry->payload.use_count() > 1) {
                        // An Observe pin still owns the payload. Keep its
                        // quota charged and retry once that pin is released.
                        if (entry != retired_tail_) {
                            retired_head_ = entry->retired_next;
                            entry->retired_next = nullptr;
                            retired_tail_->retired_next = entry;
                            retired_tail_ = entry;
                        }
                    } else {
                        retired_head_ = entry->retired_next;
                        if (!retired_head_) retired_tail_ = nullptr;
                        entry->retired_next = nullptr;
                        entry->queued = false;
                        --retired_count_;
                        owner = entry->owner;
                        words = entry->payload->size;
                        ++owner->retiring;
                        retired = std::move(entry->payload);
                    }
                }
                if (retired) {
                    retired.reset(); // last payload reference, outside mutex_
                    std::lock_guard<std::mutex> lock(mutex_);
                    owner->words -= words;
                    --owner->retiring;
                    FinishClosingLocked(*owner);
                }
            }
            bool changed = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                changed = retire_epoch_ != epoch;
                for (auto& device : devices_) FinishClosingLocked(device);
            }
            // Reentrant calls can enqueue work while a payload is destroyed.
            // A stable queue containing only pins must not spin indefinitely.
            if (!changed || !allowance) return;
        } catch (...) { return; }
    }
}

std::uint64_t VertexDeclarationRegistry::OpenDevice() noexcept {
    try {
        Drain(); // retry deferred retirements before deciding all slots are busy
        std::lock_guard<std::mutex> lock(mutex_);
        if (!next_device_) return 0;
        for (auto& device : devices_) {
            if (device.active || device.closing) continue;
            const auto id = next_device_;
            next_device_ = Advance(id);
            device.id = id;
            device.active = true;
            device.unsafe = false;
            device.next_operation = 1;
            device.next_generation = 1;
            device.pending = {};
            device.pending_handle = 0;
            device.rebind_epoch = 0;
            device.rebind_value = 0;
            device.words = 0;
            device.preparing = 0;
            device.retiring = 0;
            return id;
        }
    } catch (...) {}
    return 0;
}
void VertexDeclarationRegistry::CloseDevice(std::uint64_t id) noexcept {
    try {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Device* device = FindLocked(id);
            if (!device) return;
            device->active = false;
            device->closing = true;
            PoisonLocked(*device);
        }
        Drain();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Device* device = FindLocked(id, true);
            if (!device || !device->closing) return;
            FinishClosingLocked(*device);
        }
    } catch (...) {}
}
void VertexDeclarationRegistry::MarkUnsafe(std::uint64_t id) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        Device* device = FindLocked(id);
        if (device) PoisonLocked(*device);
    } catch (...) {}
}
void VertexDeclarationRegistry::Invalidate(std::uint64_t id) noexcept {
    try {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Device* device = FindLocked(id);
            if (!device) return;
            PoisonLocked(*device);
        }
        Drain();
    } catch (...) {}
}
bool VertexDeclarationRegistry::Matches(const Device& device, ShaderTicket ticket) const noexcept {
    return device.active && !device.unsafe && ticket && device.pending.device == ticket.device &&
           device.pending.sequence == ticket.sequence && device.pending.kind == ticket.kind;
}

ShaderTicket VertexDeclarationRegistry::Begin(std::uint64_t id, ShaderOperation kind, std::uint32_t handle) noexcept {
    ShaderTicket result;
    bool drain = false;
    try {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Device* device = FindLocked(id);
            if (!device || device->unsafe) return {};
            if (device->pending || !device->next_operation || kind == ShaderOperation::None) {
                PoisonLocked(*device);
                drain = true;
            } else {
                const auto sequence = device->next_operation;
                device->next_operation = Advance(sequence);
                device->pending = {id, sequence, kind};
                if (kind == ShaderOperation::Delete) {
                    device->rebind_epoch = 0;
                    device->pending_handle = handle;
                    Entry* entry = FindHandleLocked(*device, handle);
                    if (entry) {
                        entry->available = false;
                        entry->reset_history = false;
                        entry->reset_epoch = 0;
                        EnqueueLocked(*entry);
                        drain = true;
                    }
                } else if (kind == ShaderOperation::Set) {
                    // A failed/overlapping Set cannot keep a stale rebind proof.
                    device->rebind_epoch = 0;
                } else if (kind == ShaderOperation::Reset) {
                    device->rebind_epoch = 0;
                    for (auto& entry : device->handles) {
                        const bool keep = limits_.preserve_reset_history && entry.occupied && entry.payload &&
                            entry.scan_status == DeclarationStatus::Complete &&
                            ((entry.available && entry.reason == ShaderUnavailableReason::None) ||
                             (entry.reset_history && entry.reason == ShaderUnavailableReason::ResetAttempt));
                        if (entry.occupied) entry.reason = ShaderUnavailableReason::ResetAttempt;
                        entry.available = false;
                        entry.reset_history = keep;
                        entry.reset_epoch = keep ? sequence : 0;
                        entry.reset_hr = 0;
                        if (!keep) EnqueueLocked(entry);
                    }
                    drain = true;
                }
                result = device->pending;
            }
        }
        if (drain) Drain();
        return result;
    } catch (...) { Invalidate(id); return {}; }
}
ShaderTicket VertexDeclarationRegistry::BeginCreate(std::uint64_t id) noexcept { return Begin(id, ShaderOperation::Create, 0); }
ShaderTicket VertexDeclarationRegistry::BeginDelete(std::uint64_t id, std::uint32_t handle) noexcept { return Begin(id, ShaderOperation::Delete, handle); }
ShaderTicket VertexDeclarationRegistry::BeginSet(std::uint64_t id) noexcept { return Begin(id, ShaderOperation::Set, 0); }
ShaderTicket VertexDeclarationRegistry::BeginReset(std::uint64_t id) noexcept { return Begin(id, ShaderOperation::Reset, 0); }
ShaderTicket VertexDeclarationRegistry::BeginDraw(std::uint64_t id) noexcept { return Begin(id, ShaderOperation::Draw, 0); }

bool VertexDeclarationRegistry::EndCreate(ShaderTicket ticket, bool native_success, bool handle_known,
                                          std::uint32_t handle, DeclarationSnapshot snapshot) noexcept {
    const bool complete = snapshot.status == DeclarationStatus::Complete && !snapshot.words.empty() &&
                          snapshot.words.back() == 0xffffffffu;
    const std::size_t count = complete ? snapshot.words.size() : 0;
    bool drain = false;
    bool finished = false, completed = false;
    try {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Device* device = FindLocked(ticket.device);
            if (!device) return false;
            // A ticket can be finalized only once. While its payload is being
            // prepared outside the mutex, a second EndCreate must poison the
            // device rather than publish a competing candidate.
            if (ticket.kind != ShaderOperation::Create || !Matches(*device, ticket) || device->preparing) {
                if (!device->unsafe) { PoisonLocked(*device); drain = true; }
                finished = true;
            } else if (!native_success) {
                device->pending = {};
                return true;
            } else if (!handle_known || !device->next_generation) {
                PoisonLocked(*device); drain = true; finished = true;
            } else if (Entry* old = FindHandleLocked(*device, handle)) {
                // A repeated successful handle is ambiguous, including after
                // Reset or a failed Delete. Keep a tombstone, never replace.
                old->available = false;
                old->reason = ShaderUnavailableReason::HandleCollision;
                old->reset_history = false;
                old->reset_epoch = 0;
                old->scan_status = snapshot.status;
                EnqueueLocked(*old);
                old->generation = device->next_generation;
                device->next_generation = Advance(device->next_generation);
                device->pending = {};
                drain = true; finished = true; completed = true;
            } else if (!FreeHandleLocked(*device) ||
                       (complete && (count > limits_.words_per_declaration ||
                                     count > limits_.words_per_device ||
                                     device->words > limits_.words_per_device - count))) {
                PoisonLocked(*device); drain = true; finished = true;
            } else if (!complete) {
                Entry* entry = FreeHandleLocked(*device);
                entry->occupied = true;
                entry->available = false;
                entry->handle = handle;
                entry->generation = device->next_generation;
                entry->scan_status = snapshot.status;
                entry->reset_history = false;
                entry->reset_epoch = 0;
                entry->reason = snapshot.status == DeclarationStatus::Complete
                    ? ShaderUnavailableReason::InvalidSnapshot : ShaderUnavailableReason::ScanIncomplete;
                device->next_generation = Advance(device->next_generation);
                device->pending = {};
                return true;
            } else {
                // Reserve quota and pin this device lifetime before allocating
                // outside the lock. Close cannot reuse the slot in between.
                device->words += count;
                ++device->preparing;
                drain = false;
            }
        }
        if (finished) {
            if (drain) Drain();
            return completed;
        }

        std::shared_ptr<Payload> payload;
        try {
            payload = std::make_shared<Payload>();
            payload->size = count;
            payload->words.reset(new std::uint32_t[count]);
            std::copy(snapshot.words.begin(), snapshot.words.end(), payload->words.get());
        } catch (...) {
            payload.reset(); // complete deallocation before releasing reservation
            FinishPreparation(ticket.device, count);
            Invalidate(ticket.device);
            return false;
        }
        bool inserted = false;
        bool poisoned = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Device* device = FindLocked(ticket.device, true);
            if (!device) return false; // violated lifetime contract; no storage exposed
            if (!Matches(*device, ticket) || FindHandleLocked(*device, handle) || !FreeHandleLocked(*device) ||
                !device->preparing || device->words < count) {
                if (device->active && !device->unsafe) { PoisonLocked(*device); poisoned = true; }
            } else {
                Entry* entry = FreeHandleLocked(*device);
                entry->occupied = true;
                entry->available = true;
                entry->handle = handle;
                entry->generation = device->next_generation;
                entry->scan_status = snapshot.status;
                entry->reason = ShaderUnavailableReason::None;
                entry->reset_history = false;
                entry->reset_epoch = 0;
                entry->payload = std::move(payload); // free slot has no owned payload
                --device->preparing; // reservation now belongs to the entry
                device->next_generation = Advance(device->next_generation);
                device->pending = {};
                inserted = true;
            }
        }
        if (!inserted) {
            payload.reset(); // release outside lock while quota is still charged
            FinishPreparation(ticket.device, count);
        }
        if (poisoned) Drain();
        return inserted;
    } catch (...) { Invalidate(ticket.device); return false; }
}

bool VertexDeclarationRegistry::EndDelete(ShaderTicket ticket, bool native_success) noexcept {
    bool result = false, drain = false;
    try {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Device* device = FindLocked(ticket.device);
            if (!device) return false;
            if (ticket.kind != ShaderOperation::Delete || !Matches(*device, ticket)) {
                if (!device->unsafe) { PoisonLocked(*device); drain = true; }
            } else {
                Entry* entry = FindHandleLocked(*device, device->pending_handle);
                if (native_success && entry) {
                    entry->occupied = false;
                    entry->available = false;
                    entry->generation = 0;
                    entry->reason = ShaderUnavailableReason::None;
                    entry->scan_status = DeclarationStatus::Unreadable;
                    entry->reset_history = false;
                    entry->reset_epoch = 0;
                    EnqueueLocked(*entry);
                    drain = true;
                } else if (entry) {
                    entry->reason = ShaderUnavailableReason::DeleteFailed;
                }
                device->pending = {};
                device->pending_handle = 0;
                result = true;
            }
        }
        if (drain) Drain();
    } catch (...) { Invalidate(ticket.device); }
    return result;
}

bool VertexDeclarationRegistry::EndReset(ShaderTicket ticket, std::uint32_t hr) noexcept {
    bool result = false, drain = false;
    try {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Device* device = FindLocked(ticket.device);
            if (!device) return false;
            if (ticket.kind != ShaderOperation::Reset || !Matches(*device, ticket)) {
                if (!device->unsafe) { PoisonLocked(*device); drain = true; }
            } else {
                for (auto& entry : device->handles) {
                    if (!entry.reset_history || entry.reset_epoch != ticket.sequence) continue;
                    if (hr & 0x80000000u) {
                        entry.reset_history = false;
                        entry.reset_epoch = 0;
                        EnqueueLocked(entry);
                        drain = true;
                    } else entry.reset_hr = hr;
                }
                device->pending = {};
                result = true;
            }
        }
        if (drain) Drain();
    } catch (...) { Invalidate(ticket.device); }
    return result;
}

bool VertexDeclarationRegistry::EndSet(ShaderTicket ticket, std::uint32_t hr, std::uint32_t value) noexcept {
    bool result = false, drain = false;
    try {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Device* device = FindLocked(ticket.device);
            if (!device) return false;
            if (ticket.kind != ShaderOperation::Set || !Matches(*device, ticket)) {
                if (!device->unsafe) { PoisonLocked(*device); drain = true; }
            } else {
                if (!(hr & 0x80000000u)) {
                    device->rebind_value = value;
                    device->rebind_epoch = ticket.sequence;
                }
                device->pending = {};
                result = true;
            }
        }
        if (drain) Drain();
    } catch (...) { Invalidate(ticket.device); }
    return result;
}

bool VertexDeclarationRegistry::End(ShaderTicket ticket) noexcept {
    // A generic completion has no native HRESULT and must not authorize
    // retained data or a rebind. Existing callers still receive protocol status.
    if (ticket.kind == ShaderOperation::Reset) return EndReset(ticket, 0x80004005u);
    if (ticket.kind == ShaderOperation::Set) return EndSet(ticket, 0x80004005u, 0);
    bool result = false, drain = false;
    try {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Device* device = FindLocked(ticket.device);
            if (!device) return false;
            if (ticket.kind == ShaderOperation::Create || ticket.kind == ShaderOperation::Delete || !Matches(*device, ticket)) {
                if (!device->unsafe) { PoisonLocked(*device); drain = true; }
            } else { device->pending = {}; result = true; }
        }
        if (drain) Drain();
    } catch (...) { Invalidate(ticket.device); }
    return result;
}

ShaderObservation VertexDeclarationRegistry::Observe(ShaderTicket ticket, std::uint32_t value) noexcept {
    ShaderObservation result;
    std::shared_ptr<const Payload> pin;
    bool history = false;
    try {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Device* device = FindLocked(ticket.device);
            if (!device || ticket.kind != ShaderOperation::Draw || !Matches(*device, ticket)) {
                result.status = ShaderObservationStatus::Unsafe; return result;
            }
            Entry* entry = FindHandleLocked(*device, value);
            if (!entry) return result;
            result.generation = entry->generation;
            result.has_scan_status = true;
            result.scan_status = entry->scan_status;
            if (!entry->available || !entry->payload) {
                result.status = ShaderObservationStatus::Unavailable;
                result.reason = entry->reason == ShaderUnavailableReason::None
                    ? ShaderUnavailableReason::Unknown : entry->reason;
                history = limits_.preserve_reset_history && entry->reset_history && entry->payload &&
                    entry->reason == ShaderUnavailableReason::ResetAttempt &&
                    entry->scan_status == DeclarationStatus::Complete && entry->reset_epoch &&
                    !(entry->reset_hr & 0x80000000u) && device->rebind_value == value &&
                    device->rebind_epoch > entry->reset_epoch && device->rebind_epoch < ticket.sequence;
                if (!history) return result;
                result.reset_epoch = entry->reset_epoch;
                result.reset_hr = entry->reset_hr;
                result.rebind_epoch = device->rebind_epoch;
            }
            pin = entry->payload; // strong pin, no allocation
        }
        // Copy and allocator callbacks happen outside mutex_. The source pin
        // keeps the quota charged and the device slot alive until released.
        auto& destination = history ? result.pre_reset_words : result.words;
        destination.assign(pin->words.get(), pin->words.get() + pin->size);
        bool still_valid = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Device* device = FindLocked(ticket.device);
            Entry* entry = device ? FindHandleLocked(*device, value) : nullptr;
            still_valid = device && Matches(*device, ticket) && entry &&
                          entry->generation == result.generation && entry->payload == pin;
            if (still_valid && history)
                still_valid = !entry->available && entry->reset_history &&
                    entry->reason == ShaderUnavailableReason::ResetAttempt &&
                    entry->scan_status == DeclarationStatus::Complete &&
                    entry->reset_epoch == result.reset_epoch && entry->reset_hr == result.reset_hr &&
                    device->rebind_value == value && device->rebind_epoch == result.rebind_epoch;
            else if (still_valid) still_valid = entry->available;
        }
        if (!still_valid) {
            std::vector<std::uint32_t>().swap(destination);
            result.status = ShaderObservationStatus::Unsafe;
        } else if (!history) result.status = ShaderObservationStatus::Candidate;
    } catch (...) {
        std::vector<std::uint32_t>().swap(result.words);
        std::vector<std::uint32_t>().swap(result.pre_reset_words);
        if (!history) {
            result.status = ShaderObservationStatus::Unavailable;
            result.reason = ShaderUnavailableReason::ObservationError;
        }
    }
    if (pin) {
        pin.reset();
        bool drain = false;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            drain = retired_count_ != 0;
            if (drain) ++retire_epoch_; // wake a reentrant outer drain
        } catch (...) {}
        if (drain) Drain();
    }
    return result;
}

} // namespace t3render::ir
