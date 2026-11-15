#pragma once

// Optional read-only candidate for the local StaticMesh gamesys property.
// This does not call game code, follow inherited defaults, or prove a GPU draw.
// Call only while an independently validated owner is live in the observation.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace t3render::scene_bridge {

struct AssetReader {
    void* context = nullptr;
    bool (*read)(void*, std::uint32_t, void*, std::size_t) noexcept = nullptr;
};

enum class AssetStatus {
    Unknown, Unreadable, DescriptorUnsupported, LocalPropertyMissing,
    PropertyMalformed, ObjectNotUnique, ProxyMismatch, ResourceMismatch, Candidate
};

// Raw values from the validated property path, for diagnostic trace auditing
// only. Transient addresses must never be used as SceneIR identifiers.
constexpr std::size_t kMaxAssetAuditSlots = 256;
struct AssetAuditSlot {
    std::uint32_t index = 0, next = 0, id_flags = 0, value = 0;
};
struct AssetAudit {
    bool available = false;
    std::uint32_t descriptor_table = 0, descriptor_array = 0, descriptor = 0;
    std::uint32_t descriptor_vtable = 0, descriptor_copy = 0;
    std::uint32_t descriptor_count = 0, descriptor_size = 0, descriptor_flags = 0;
    std::uint32_t owner_flags = 0, property_hash = 0, bucket_head = 0;
    std::uint32_t slot_index = 0, slot_next = 0, slot_id_flags = 0, slot_value = 0;
    std::uint32_t chain_count = 0;
    AssetAuditSlot chain[kMaxAssetAuditSlots]{};
    std::uint32_t objects_data = 0, objects_count = 0, objects_capacity = 0, object_index = 0;
    std::uint32_t proxy = 0, proxy_vtable = 0, proxy_index = 0, resource = 0, resource_vtable = 0;
    std::uint32_t name_address = 0, name_length = 0;
};

struct AssetCandidate {
    AssetStatus status = AssetStatus::Unknown;
    bool descriptor_match = false;
    bool local_property_found = false;
    bool proxy_live_member = false;
    bool proxy_vtable_match = false;
    bool resource_vtable_match = false;
    bool stable = false;
    bool name_available = false;
    char name[128]{};
    AssetAudit audit;
};

namespace asset_detail {
constexpr std::uint32_t kPropertyId = 0x2b;
constexpr std::uint32_t kPool = 0x10f7b5c8;
constexpr std::uint32_t kPoolSlots = 61440;
constexpr std::uint32_t kObjects = 0x10f3e4a0;
constexpr std::size_t kMaxChain = kMaxAssetAuditSlots;

inline bool offset(std::uint32_t base, std::uint32_t add, std::uint32_t& out) noexcept {
    if (!base || base > std::numeric_limits<std::uint32_t>::max() - add) return false;
    out = base + add;
    return true;
}

inline bool read(const AssetReader& r, std::uint32_t address, void* out, std::size_t size) noexcept {
    return r.read && address && out && size && size <= (std::uint64_t{1} << 32) - address &&
           r.read(r.context, address, out, size);
}

template<class T> inline bool get(const AssetReader& r, std::uint32_t base, std::uint32_t add, T& out) noexcept {
    std::uint32_t address = 0;
    return offset(base, add, address) && read(r, address, &out, sizeof(out));
}

struct Descriptor {
    std::uint32_t table = 0, array = 0, property = 0, vtable = 0, copy = 0;
    std::uint32_t count = 0, size = 0, flags = 0;
};

inline bool descriptor(const AssetReader& r, Descriptor& d) noexcept {
    return read(r, 0x10f3eda4, &d.table, 4) && get(r, d.table, 8, d.array) &&
           get(r, d.array, kPropertyId * 4, d.property) && get(r, d.property, 0, d.vtable) &&
           get(r, d.vtable, 0xb4, d.copy) && get(r, d.property, 0x34, d.count) &&
           get(r, d.property, 0x38, d.size) && get(r, d.property, 0x3c, d.flags);
}

inline bool supported(const Descriptor& d) noexcept {
    // UObjectProperty::copy uses one 4-byte element in the no-create branch.
    return d.vtable == 0x10e72e48 && d.copy == 0x10aeecc0 && d.count == 1 && d.size == 4 &&
           (d.flags & 0x400000) == 0;
}

struct Slot { std::uint16_t next = 0, id_flags = 0; std::uint32_t value = 0; };
static_assert(sizeof(Slot) == 8, "gamesys slot layout must be eight bytes");
struct LocalProperty {
    std::uint32_t owner_flags = 0, hash = 0, proxy = 0;
    std::uint16_t head = 0;
    std::uint16_t indices[kMaxChain]{};
    Slot slots[kMaxChain]{};
    std::size_t count = 0;
    bool found = false;
};

enum class LocalStatus { Good, Missing, Unreadable, Malformed };

inline LocalStatus local_property(const AssetReader& r, std::uint32_t owner, LocalProperty& p) noexcept {
    if (!get(r, owner, 0x1c, p.owner_flags) || !get(r, owner, 0x28, p.hash)) return LocalStatus::Unreadable;
    // 0x10ad3df0 excludes objects with this flag and can fall back to class
    // defaults; this probe intentionally resolves only the local hash.
    if (p.owner_flags & 0x80000000u) return LocalStatus::Malformed;
    if (!p.hash) return LocalStatus::Missing;
    if (!get(r, p.hash, (kPropertyId & 7) * 2, p.head)) return LocalStatus::Unreadable;
    std::uint16_t index = p.head;
    std::uint16_t previous_id = 0;
    while (index) {
        if (index >= kPoolSlots || p.count == kMaxChain) return LocalStatus::Malformed;
        for (std::size_t i = 0; i < p.count; ++i) if (p.indices[i] == index) return LocalStatus::Malformed;
        Slot slot{};
        if (!get(r, kPool, static_cast<std::uint32_t>(index) * 8, slot)) return LocalStatus::Unreadable;
        const auto id = static_cast<std::uint16_t>(slot.id_flags & 0x7fffu);
        // The native lookup stops as soon as a larger ID is encountered.
        // Reject unordered or duplicated chains instead of finding a value
        // that the real getter would never reach.
        if (p.count && id <= previous_id) return LocalStatus::Malformed;
        previous_id = id;
        p.indices[p.count] = index;
        p.slots[p.count++] = slot;
        if (id == kPropertyId) {
            if (p.found) return LocalStatus::Malformed;
            p.found = true;
            if (slot.id_flags & 0x8000u) p.proxy = slot.value;
            else if (!read(r, slot.value, &p.proxy, 4)) return LocalStatus::Unreadable;
        }
        index = slot.next;
    }
    return p.found ? LocalStatus::Good : LocalStatus::Missing;
}

inline bool same_property(const LocalProperty& a, const LocalProperty& b) noexcept {
    if (a.owner_flags != b.owner_flags || a.hash != b.hash || a.head != b.head || a.proxy != b.proxy ||
        a.count != b.count || a.found != b.found) return false;
    for (std::size_t i = 0; i < a.count; ++i)
        if (a.indices[i] != b.indices[i] || std::memcmp(&a.slots[i], &b.slots[i], sizeof(Slot)) != 0) return false;
    return true;
}

struct Array { std::uint32_t data = 0, count = 0, capacity = 0; };
inline bool same_array(const Array& a, const Array& b) noexcept {
    return a.data == b.data && a.count == b.count && a.capacity == b.capacity;
}

// Finds a unique proxy slot before dereferencing it; verifies the selected slot
// and array header afterwards. ABA and unobserved concurrent mutation remain
// possible, hence the result is a candidate, never a stable identity.
inline bool proxy_member(const AssetReader& r, std::uint32_t proxy, Array& table, std::uint32_t& index) noexcept {
    if (!proxy || !read(r, kObjects, &table, sizeof(table)) || table.count > 65536 ||
        table.count > table.capacity || table.capacity > 262144 || (table.count && !table.data)) return false;
    bool found = false;
    for (std::uint32_t start = 0; start < table.count;) {
        std::uint32_t chunk[128]{};
        const auto n = table.count - start < 128 ? table.count - start : 128;
        std::uint32_t address = 0;
        if (!offset(table.data, start * 4, address) || !read(r, address, chunk, n * 4)) return false;
        for (std::uint32_t i = 0; i < n; ++i) if (chunk[i] == proxy) {
            if (found) return false;
            index = start + i;
            found = true;
        }
        start += n;
    }
    Array after{};
    std::uint32_t slot = 0;
    return found && read(r, kObjects, &after, sizeof(after)) && same_array(table, after) &&
           get(r, table.data, index * 4, slot) && slot == proxy;
}
}  // namespace asset_detail

inline AssetCandidate InspectAssetCandidate(const AssetReader& r, std::uint32_t owner) noexcept {
    using namespace asset_detail;
    AssetCandidate result;
    if (!r.read || !owner) return result;
    Descriptor descriptor_before{};
    if (!descriptor(r, descriptor_before)) { result.status = AssetStatus::Unreadable; return result; }
    if (!supported(descriptor_before)) { result.status = AssetStatus::DescriptorUnsupported; return result; }
    result.descriptor_match = true;
    LocalProperty property{};
    const LocalStatus local = local_property(r, owner, property);
    if (local != LocalStatus::Good) {
        result.status = local == LocalStatus::Missing ? AssetStatus::LocalPropertyMissing :
            local == LocalStatus::Unreadable ? AssetStatus::Unreadable : AssetStatus::PropertyMalformed;
        return result;
    }
    result.local_property_found = true;
    Array objects{};
    std::uint32_t index = 0;
    if (!proxy_member(r, property.proxy, objects, index)) { result.status = AssetStatus::ObjectNotUnique; return result; }
    result.proxy_live_member = true;
    std::uint32_t proxy_vtable = 0, proxy_index = 0, resource = 0, resource_vtable = 0, string = 0, length = 0;
    if (!get(r, property.proxy, 0, proxy_vtable) || !get(r, property.proxy, 4, proxy_index) ||
        proxy_vtable != 0x10e4dfa8 || proxy_index != index) { result.status = AssetStatus::ProxyMismatch; return result; }
    result.proxy_vtable_match = true;
    if (!get(r, property.proxy, 0x58, resource) || !get(r, resource, 0, resource_vtable) ||
        resource_vtable != 0x10ec0e20) { result.status = AssetStatus::ResourceMismatch; return result; }
    result.resource_vtable_match = true;
    if (!get(r, resource, 8, string) || string < 4 || !read(r, string - 4, &length, 4) ||
        !length || length >= sizeof(result.name)) { result.status = AssetStatus::Unreadable; return result; }
    char name[sizeof(result.name)]{};
    if (!read(r, string, name, static_cast<std::size_t>(length) + 1) || name[length] != '\0') {
        result.status = AssetStatus::Unreadable; return result;
    }
    for (std::uint32_t i = 0; i < length; ++i) if (static_cast<unsigned char>(name[i]) < 32 ||
        static_cast<unsigned char>(name[i]) >= 127) { result.status = AssetStatus::Unreadable; return result; }

    Descriptor descriptor_after{};
    LocalProperty property_after{};
    Array objects_after{};
    std::uint32_t slot_after = 0, proxy_vtable_after = 0, index_after = 0, resource_after = 0;
    std::uint32_t resource_vtable_after = 0, string_after = 0, length_after = 0;
    char name_after[sizeof(name)]{};
    if (!descriptor(r, descriptor_after) || std::memcmp(&descriptor_before, &descriptor_after, sizeof(Descriptor)) != 0 ||
        local_property(r, owner, property_after) != LocalStatus::Good || !same_property(property, property_after) ||
        !read(r, kObjects, &objects_after, sizeof(objects_after)) || !same_array(objects, objects_after) ||
        !get(r, objects.data, index * 4, slot_after) || slot_after != property.proxy ||
        !get(r, property.proxy, 0, proxy_vtable_after) || proxy_vtable_after != proxy_vtable ||
        !get(r, property.proxy, 4, index_after) || index_after != index ||
        !get(r, property.proxy, 0x58, resource_after) || resource_after != resource ||
        !get(r, resource, 0, resource_vtable_after) || resource_vtable_after != resource_vtable ||
        !get(r, resource, 8, string_after) || string_after != string ||
        !read(r, string - 4, &length_after, 4) || length_after != length ||
        !read(r, string, name_after, static_cast<std::size_t>(length) + 1) ||
        std::memcmp(name, name_after, static_cast<std::size_t>(length) + 1) != 0) {
        result.status = AssetStatus::Unknown; return result;
    }
    std::memcpy(result.name, name, static_cast<std::size_t>(length) + 1);
    auto& audit = result.audit;
    audit.descriptor_table = descriptor_before.table;
    audit.descriptor_array = descriptor_before.array;
    audit.descriptor = descriptor_before.property;
    audit.descriptor_vtable = descriptor_before.vtable;
    audit.descriptor_copy = descriptor_before.copy;
    audit.descriptor_count = descriptor_before.count;
    audit.descriptor_size = descriptor_before.size;
    audit.descriptor_flags = descriptor_before.flags;
    audit.owner_flags = property.owner_flags;
    audit.property_hash = property.hash;
    audit.bucket_head = property.head;
    audit.chain_count = static_cast<std::uint32_t>(property.count);
    for (std::size_t i = 0; i < property.count; ++i) {
        audit.chain[i] = {property.indices[i], property.slots[i].next,
                          property.slots[i].id_flags, property.slots[i].value};
        if ((property.slots[i].id_flags & 0x7fffu) == kPropertyId) {
            audit.slot_index = property.indices[i];
            audit.slot_next = property.slots[i].next;
            audit.slot_id_flags = property.slots[i].id_flags;
            audit.slot_value = property.slots[i].value;
        }
    }
    audit.objects_data = objects.data;
    audit.objects_count = objects.count;
    audit.objects_capacity = objects.capacity;
    audit.object_index = index;
    audit.proxy = property.proxy;
    audit.proxy_vtable = proxy_vtable;
    audit.proxy_index = proxy_index;
    audit.resource = resource;
    audit.resource_vtable = resource_vtable;
    audit.name_address = string;
    audit.name_length = length;
    audit.available = true;
    result.stable = true;
    result.name_available = true;
    result.status = AssetStatus::Candidate;
    return result;
}
}  // namespace t3render::scene_bridge
