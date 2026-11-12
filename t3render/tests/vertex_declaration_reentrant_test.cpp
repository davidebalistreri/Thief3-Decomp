#include "../ir/vertex_declaration_registry.hpp"

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <new>
#include <thread>
#include <utility>

namespace {
using Hook = void (*)() noexcept;
std::atomic<Hook> allocation_hook{nullptr}, deallocation_hook{nullptr};
std::atomic<bool> fail_allocation{false};

void RunHook(std::atomic<Hook>& source) noexcept {
    Hook hook = source.exchange(nullptr);
    if (!hook) return;
    hook();
}
} // namespace

void* operator new(std::size_t size) {
    if (fail_allocation.exchange(false)) throw std::bad_alloc();
    void* result = std::malloc(size ? size : 1);
    if (!result) throw std::bad_alloc();
    RunHook(allocation_hook);
    return result;
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* ptr) noexcept { RunHook(deallocation_hook); std::free(ptr); }
void operator delete[](void* ptr) noexcept { ::operator delete(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { ::operator delete(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { ::operator delete(ptr); }

namespace {
using namespace t3render::ir;
VertexDeclarationRegistry* active_registry = nullptr;
std::uint64_t active_device = 0, opened_device = 0;
std::atomic<bool> copy_entered{false}, copy_may_continue{false};
std::uint64_t other_device = 0;
unsigned callback_depth = 0, max_callback_depth = 0, callback_count = 0;
VertexDeclarationRegistry* other_registry = nullptr;
ShaderTicket duplicate_ticket;
DeclarationSnapshot* duplicate_snapshot = nullptr;
bool duplicate_result = true;

void OpenAnotherDevice() noexcept { opened_device = active_registry->OpenDevice(); }
void InvalidateActiveDevice() noexcept { active_registry->Invalidate(active_device); }
void CloseActiveDevice() noexcept { active_registry->CloseDevice(active_device); }
void SetDuringCreate() noexcept { (void)active_registry->BeginSet(active_device); }
void PauseDuringCopy() noexcept {
    copy_entered = true;
    while (!copy_may_continue.load()) std::this_thread::yield();
}
void CloseOtherOnEveryFree() noexcept {
    ++callback_depth;
    if (callback_depth > max_callback_depth) max_callback_depth = callback_depth;
    ++callback_count;
    deallocation_hook = CloseOtherOnEveryFree;
    active_registry->CloseDevice(other_device);
    --callback_depth;
}
void AlternateRegistriesOnEveryFree() noexcept {
    ++callback_depth;
    if (callback_depth > max_callback_depth) max_callback_depth = callback_depth;
    ++callback_count;
    deallocation_hook = AlternateRegistriesOnEveryFree;
    if (callback_depth == 1) other_registry->CloseDevice(other_device);
    else active_registry->Invalidate(active_device);
    --callback_depth;
}
void DuplicateEndCreate() noexcept {
    duplicate_result = active_registry->EndCreate(duplicate_ticket, true, true, 77, std::move(*duplicate_snapshot));
}

DeclarationSnapshot Snapshot() {
    return {DeclarationStatus::Complete, {0x20000000u, 0x40020000u, 0xffffffffu}};
}
void Register(VertexDeclarationRegistry& registry, std::uint64_t device, std::uint32_t handle) {
    auto snapshot = Snapshot();
    const auto ticket = registry.BeginCreate(device);
    assert(ticket && registry.EndCreate(ticket, true, true, handle, std::move(snapshot)));
}

void AllocationCanReenterWithoutDeadlock() {
    VertexDeclarationRegistry registry({2, 4, 8, 32});
    active_registry = &registry;
    active_device = registry.OpenDevice();
    opened_device = 0;
    auto snapshot = Snapshot();
    const auto ticket = registry.BeginCreate(active_device);
    allocation_hook = OpenAnotherDevice;
    assert(registry.EndCreate(ticket, true, true, 1, std::move(snapshot)));
    assert(opened_device && opened_device != active_device && !allocation_hook.load());
    const auto draw = registry.BeginDraw(active_device);
    assert(draw && registry.Observe(draw, 1).status == ShaderObservationStatus::Candidate && registry.End(draw));
    registry.CloseDevice(opened_device);
}

void ReentrantInvalidationDiscardsCopiedObservation() {
    VertexDeclarationRegistry registry({1, 4, 8, 32});
    active_registry = &registry;
    active_device = registry.OpenDevice();
    Register(registry, active_device, 2);
    const auto draw = registry.BeginDraw(active_device);
    assert(draw);
    allocation_hook = InvalidateActiveDevice;
    auto observed = registry.Observe(draw, 2);
    assert(!allocation_hook.load());
    assert(observed.status == ShaderObservationStatus::Unsafe && observed.words.empty());
    assert(!registry.End(draw) && !registry.BeginDraw(active_device));
}

void ObservationAllocationFailureHasGenerationReason() {
    VertexDeclarationRegistry registry({1, 4, 8, 32});
    const auto device = registry.OpenDevice();
    Register(registry, device, 98);
    const auto draw = registry.BeginDraw(device);
    assert(draw);
    fail_allocation = true;
    const auto observation = registry.Observe(draw, 98);
    assert(!fail_allocation.load() && observation.status == ShaderObservationStatus::Unavailable &&
           observation.reason == ShaderUnavailableReason::ObservationError && observation.has_scan_status &&
           observation.scan_status == DeclarationStatus::Complete && observation.generation && observation.words.empty());
    assert(registry.End(draw));
    const auto next = registry.BeginDraw(device);
    assert(next && registry.Observe(next, 98).status == ShaderObservationStatus::Candidate && registry.End(next));
}

void MarkUnsafeDefersDeallocation() {
    VertexDeclarationRegistry registry({2, 4, 8, 32});
    active_registry = &registry;
    active_device = registry.OpenDevice();
    Register(registry, active_device, 99);
    const auto draw = registry.BeginDraw(active_device);
    assert(draw);
    opened_device = 0;
    deallocation_hook = OpenAnotherDevice;
    registry.MarkUnsafe(active_device);
    assert(deallocation_hook.load() == OpenAnotherDevice && !opened_device);
    assert(!registry.End(draw) && !registry.BeginDraw(active_device));
    assert(deallocation_hook.load() == OpenAnotherDevice && !opened_device);
    registry.Invalidate(active_device);
    assert(!deallocation_hook.load() && opened_device && opened_device != active_device);
    registry.CloseDevice(opened_device);
}

void DeallocationCanReenterWithoutDeadlock() {
    VertexDeclarationRegistry registry({2, 4, 8, 32});
    active_registry = &registry;
    active_device = registry.OpenDevice();
    Register(registry, active_device, 3);
    opened_device = 0;
    deallocation_hook = OpenAnotherDevice;
    const auto deletion = registry.BeginDelete(active_device, 3);
    assert(deletion && opened_device && opened_device != active_device && !deallocation_hook.load());
    assert(registry.EndDelete(deletion, true));
    registry.CloseDevice(opened_device);
}

void ReentrantCloseDuringResetKeepsOldTicketInvalid() {
    VertexDeclarationRegistry registry({2, 4, 8, 32});
    active_registry = &registry;
    active_device = registry.OpenDevice();
    Register(registry, active_device, 4);
    Register(registry, active_device, 5);
    deallocation_hook = CloseActiveDevice;
    const auto reset = registry.BeginReset(active_device);
    assert(reset && !deallocation_hook.load());
    assert(!registry.End(reset));
    const auto replacement = registry.OpenDevice();
    assert(replacement && replacement != active_device);
    assert(!registry.BeginDraw(active_device));
    registry.CloseDevice(replacement);
}

void AllocationFailurePoisonsOnlyTheDevice() {
    VertexDeclarationRegistry registry({2, 4, 8, 32});
    const auto first = registry.OpenDevice(), second = registry.OpenDevice();
    auto snapshot = Snapshot();
    const auto ticket = registry.BeginCreate(first);
    fail_allocation = true;
    assert(!registry.EndCreate(ticket, true, true, 6, std::move(snapshot)));
    assert(!registry.BeginDraw(first));
    Register(registry, second, 6);
    const auto draw = registry.BeginDraw(second);
    assert(draw && registry.Observe(draw, 6).status == ShaderObservationStatus::Candidate && registry.End(draw));
}

void ReentrantMutationDuringCreatePoisonsObservation() {
    VertexDeclarationRegistry registry({1, 4, 8, 32});
    active_registry = &registry;
    active_device = registry.OpenDevice();
    auto snapshot = Snapshot();
    const auto ticket = registry.BeginCreate(active_device);
    allocation_hook = SetDuringCreate;
    assert(!registry.EndCreate(ticket, true, true, 7, std::move(snapshot)));
    assert(!allocation_hook.load() && !registry.BeginDraw(active_device));
}

void PinnedCopySurvivesConcurrentCloseAndSlotReuse() {
    VertexDeclarationRegistry registry({1, 4, 8, 32});
    active_registry = &registry;
    active_device = registry.OpenDevice();
    Register(registry, active_device, 8);
    const auto draw = registry.BeginDraw(active_device);
    assert(draw);
    std::atomic<bool> ready{false}, begin{false};
    ShaderObservation result;
    std::thread thread([&] {
        ready = true;
        while (!begin.load()) std::this_thread::yield();
        result = registry.Observe(draw, 8);
    });
    while (!ready.load()) std::this_thread::yield();
    copy_entered = false;
    copy_may_continue = false;
    allocation_hook = PauseDuringCopy;
    begin = true;
    while (!copy_entered.load()) std::this_thread::yield();
    registry.CloseDevice(active_device);
    assert(!registry.OpenDevice()); // pinned payload keeps old device slot charged
    copy_may_continue = true;
    thread.join();
    assert(result.status == ShaderObservationStatus::Unsafe && result.words.empty());
    assert(!registry.End(draw));
    const auto replacement = registry.OpenDevice();
    assert(replacement && replacement != active_device);
    const auto new_draw = registry.BeginDraw(replacement);
    assert(new_draw && registry.Observe(new_draw, 8).status == ShaderObservationStatus::Unknown && registry.End(new_draw));
    registry.CloseDevice(replacement);
}

void PreparationReservationBlocksCloseAndReuse() {
    VertexDeclarationRegistry registry({1, 4, 8, 3});
    active_device = registry.OpenDevice();
    auto snapshot = Snapshot();
    const auto ticket = registry.BeginCreate(active_device);
    std::atomic<bool> ready{false}, begin{false};
    bool result = true;
    std::thread thread([&] {
        ready = true;
        while (!begin.load()) std::this_thread::yield();
        result = registry.EndCreate(ticket, true, true, 9, std::move(snapshot));
    });
    while (!ready.load()) std::this_thread::yield();
    copy_entered = false;
    copy_may_continue = false;
    allocation_hook = PauseDuringCopy;
    begin = true;
    while (!copy_entered.load()) std::this_thread::yield();
    registry.CloseDevice(active_device);
    assert(!registry.OpenDevice()); // reserved allocation has not finished
    copy_may_continue = true;
    thread.join();
    assert(!result);
    const auto replacement = registry.OpenDevice();
    assert(replacement && replacement != active_device);
    Register(registry, replacement, 10); // quota was returned only after destruction
    registry.CloseDevice(replacement);
}

void DuplicateCreateCompletionPoisonsDevice() {
    VertexDeclarationRegistry registry({1, 4, 8, 32});
    active_registry = &registry;
    active_device = registry.OpenDevice();
    auto first = Snapshot();
    auto second = Snapshot();
    duplicate_ticket = registry.BeginCreate(active_device);
    duplicate_snapshot = &second;
    duplicate_result = true;
    allocation_hook = DuplicateEndCreate;
    assert(!registry.EndCreate(duplicate_ticket, true, true, 76, std::move(first)));
    assert(!duplicate_result && !allocation_hook.load() && !registry.BeginDraw(active_device));

    // Repeat with a separate thread blocked in allocation to exercise the
    // same ticket race without relying on callback ordering.
    registry.CloseDevice(active_device);
    active_device = registry.OpenDevice();
    first = Snapshot(); second = Snapshot();
    const auto ticket = registry.BeginCreate(active_device);
    std::atomic<bool> ready{false}, begin{false};
    bool first_result = true;
    std::thread thread([&] {
        ready = true;
        while (!begin.load()) std::this_thread::yield();
        first_result = registry.EndCreate(ticket, true, true, 76, std::move(first));
    });
    while (!ready.load()) std::this_thread::yield();
    copy_entered = false;
    copy_may_continue = false;
    allocation_hook = PauseDuringCopy;
    begin = true;
    while (!copy_entered.load()) std::this_thread::yield();
    assert(!registry.EndCreate(ticket, true, true, 77, std::move(second)));
    assert(!registry.BeginDraw(active_device));
    copy_may_continue = true;
    thread.join();
    assert(!first_result && !registry.BeginDraw(active_device));
}

void RetirementBlocksCloseUntilActualDeallocation() {
    VertexDeclarationRegistry registry({1, 4, 8, 3});
    active_device = registry.OpenDevice();
    Register(registry, active_device, 11);
    std::atomic<bool> ready{false}, begin{false};
    ShaderTicket deletion;
    std::thread thread([&] {
        ready = true;
        while (!begin.load()) std::this_thread::yield();
        deletion = registry.BeginDelete(active_device, 11);
    });
    while (!ready.load()) std::this_thread::yield();
    copy_entered = false;
    copy_may_continue = false;
    deallocation_hook = PauseDuringCopy;
    begin = true;
    while (!copy_entered.load()) std::this_thread::yield();
    registry.CloseDevice(active_device);
    assert(!registry.OpenDevice()); // deallocator still owns the charged payload
    copy_may_continue = true;
    thread.join();
    assert(deletion && !registry.EndDelete(deletion, true));
    const auto replacement = registry.OpenDevice();
    assert(replacement && replacement != active_device);
    Register(registry, replacement, 12);
    registry.CloseDevice(replacement);
}

void FailedMetadataAllocationNeverPublishesDevice() {
    fail_allocation = true;
    VertexDeclarationRegistry registry({1, 4, 8, 32});
    assert(!fail_allocation.load() && !registry.OpenDevice());
}

void PersistentReentrantFreeDrainsAndFinalizesOtherDevice() {
    VertexDeclarationRegistry registry({2, 64, 8, 512});
    active_registry = &registry;
    active_device = registry.OpenDevice();
    other_device = registry.OpenDevice();
    assert(active_device && other_device);
    for (unsigned i = 0; i < 32; ++i) {
        Register(registry, active_device, i);
        Register(registry, other_device, i);
    }
    callback_count = callback_depth = max_callback_depth = 0;
    deallocation_hook = CloseOtherOnEveryFree;
    registry.Invalidate(active_device);
    deallocation_hook = nullptr;
    assert(callback_count >= 32 && max_callback_depth == 1);
    assert(!registry.BeginDraw(active_device) && !registry.BeginDraw(other_device));
    const auto replacement = registry.OpenDevice();
    assert(replacement && replacement != other_device);
    registry.CloseDevice(replacement);
}

void CrossRegistryRecursiveFreeIsBounded() {
    VertexDeclarationRegistry first({1, 32, 8, 256}), second({1, 32, 8, 256});
    active_registry = &first;
    other_registry = &second;
    active_device = first.OpenDevice();
    other_device = second.OpenDevice();
    for (unsigned i = 0; i < 16; ++i) {
        Register(first, active_device, i);
        Register(second, other_device, i);
    }
    callback_count = callback_depth = max_callback_depth = 0;
    deallocation_hook = AlternateRegistriesOnEveryFree;
    first.Invalidate(active_device);
    deallocation_hook = nullptr;
    assert(callback_count >= 32 && max_callback_depth == 2);
    assert(!second.BeginDraw(other_device));
    const auto replacement = second.OpenDevice();
    assert(replacement && replacement != other_device);
    second.CloseDevice(replacement);
}
} // namespace

int main() {
    AllocationCanReenterWithoutDeadlock();
    ReentrantInvalidationDiscardsCopiedObservation();
    ObservationAllocationFailureHasGenerationReason();
    MarkUnsafeDefersDeallocation();
    DeallocationCanReenterWithoutDeadlock();
    ReentrantCloseDuringResetKeepsOldTicketInvalid();
    AllocationFailurePoisonsOnlyTheDevice();
    ReentrantMutationDuringCreatePoisonsObservation();
    PinnedCopySurvivesConcurrentCloseAndSlotReuse();
    PreparationReservationBlocksCloseAndReuse();
    DuplicateCreateCompletionPoisonsDevice();
    RetirementBlocksCloseUntilActualDeallocation();
    FailedMetadataAllocationNeverPublishesDevice();
    PersistentReentrantFreeDrainsAndFinalizesOtherDevice();
    CrossRegistryRecursiveFreeIsBounded();
}
