#pragma once

#include "scene_bridge_probe.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace t3render::scene_bridge {

// A fault-safe reader is supplied by the platform; tests use an isolated map.
// A partial read must return false and not expose a partial value.
struct ProbeReader {
    void* context = nullptr;
    bool (*read)(void*, std::uint32_t, void*, std::size_t) noexcept = nullptr;
};

inline bool ReadProbe(const ProbeReader& reader, std::uint32_t address,
                      void* destination, std::size_t size) noexcept {
    constexpr std::uint64_t kEnd = std::uint64_t{1} << 32;
    return reader.read && address && destination && size &&
           size <= kEnd - address && reader.read(reader.context, address, destination, size);
}

inline ProbeValue ReadProbeU32(const ProbeReader& reader, std::uint32_t address) noexcept {
    ProbeValue result;
    std::uint32_t value = 0;
    if (ReadProbe(reader, address, &value, sizeof(value))) {
        result.value = value;
        result.available = true;
    }
    return result;
}

inline bool ProbeOffset(std::uint32_t address, std::uint32_t offset, std::uint32_t& result) noexcept {
    if (!address || address > std::numeric_limits<std::uint32_t>::max() - offset) return false;
    result = address + offset;
    return true;
}

inline ProbeValue ReadProbeOffset(const ProbeReader& reader, std::uint32_t base, std::uint32_t offset) noexcept {
    std::uint32_t address = 0;
    return ProbeOffset(base, offset, address) ? ReadProbeU32(reader, address) : ProbeValue{};
}

inline bool ReadProbeBytesOffset(const ProbeReader& reader, std::uint32_t base, std::uint32_t offset,
                                 void* destination, std::size_t size) noexcept {
    std::uint32_t address = 0;
    return ProbeOffset(base, offset, address) && ReadProbe(reader, address, destination, size);
}

inline bool ReadMeshNodeName(const ProbeReader& reader, std::uint32_t address, char (&name)[128]) noexcept {
    for (std::uint32_t i = 0; i < sizeof(name); ++i) {
        std::uint32_t at = 0;
        unsigned char c = 0;
        if (!ProbeOffset(address, i, at) || !ReadProbe(reader, at, &c, 1)) return false;
        if (!c) { name[i] = '\0'; return i != 0; }
        if (c < 32 || c >= 127 || i + 1 == sizeof(name)) return false;
        name[i] = static_cast<char>(c);
    }
    return false;
}

// cString stores a byte length immediately before its character buffer.
// Null/empty strings are represented as unavailable, not as material names.
inline bool ReadMeshSectionName(const ProbeReader& reader, std::uint32_t address,
                                char (&name)[128], bool& available) noexcept {
    available = false;
    if (!address) return true;
    if (address < 4) return false;
    const auto size = ReadProbeU32(reader, address - 4);
    if (!size.available || size.value >= sizeof(name)) return false;
    char first[128]{}, after[128]{};
    if (!ReadProbe(reader, address, first, size.value + 1) || first[size.value] != '\0') return false;
    for (std::uint32_t i = 0; i < size.value; ++i)
        if (static_cast<unsigned char>(first[i]) < 32 || static_cast<unsigned char>(first[i]) >= 127) return false;
    const auto size_after = ReadProbeU32(reader, address - 4);
    if (!size_after.available || size_after.value != size.value ||
        !ReadProbe(reader, address, after, size.value + 1) || std::memcmp(first, after, size.value + 1) != 0) return false;
    if (size.value) { std::memcpy(name, first, size.value + 1); available = true; }
    return true;
}

inline void ReadMeshNodeSections(const ProbeReader& reader, std::uint32_t element, ProbeMeshNode& node) noexcept {
    if (!node.stable) return;
    if (node.section_count > kMaxMeshNodeSections) { node.sections_truncated = true; return; }
    if (!node.section_count) { node.sections_available = true; return; }
    node.section_array = ReadProbeOffset(reader, node.address.value, 0x30);
    node.instance_materials = ReadProbeOffset(reader, element, 0x104);
    if (!node.section_array.available || !node.section_array.value || !node.instance_materials.available ||
        node.instance_materials.value < 4) return;
    const auto material_count = ReadProbeU32(reader, node.instance_materials.value - 4);
    if (!material_count.available || material_count.value != node.section_count) return;
    std::uint32_t sections[kMaxMeshNodeSections][9]{}, materials[kMaxMeshNodeSections][3]{};
    std::uint32_t sections_after[kMaxMeshNodeSections][9]{}, materials_after[kMaxMeshNodeSections][3]{};
    const auto section_bytes = node.section_count * sizeof(sections[0]);
    const auto material_bytes = node.section_count * sizeof(materials[0]);
    if (!ReadProbe(reader, node.section_array.value, sections, section_bytes) ||
        !ReadProbe(reader, node.instance_materials.value, materials, material_bytes)) return;
    for (std::uint32_t i = 0; i < node.section_count; ++i) {
        auto& out = node.sections[i];
        out.triangles = sections[i][3]; out.first_vertex = sections[i][4]; out.vertex_count = sections[i][5];
        out.first_index = sections[i][6]; out.index_count = sections[i][7]; out.unknown = sections[i][8];
        if (std::uint64_t{out.first_vertex} + out.vertex_count > node.vertex_count ||
            std::uint64_t{out.first_index} + out.index_count > node.index_count ||
            !ReadMeshSectionName(reader, sections[i][0], out.name, out.name_available) ||
            !ReadMeshSectionName(reader, materials[i][0], out.material_name, out.material_name_available)) return;
    }
    const auto node_after = ReadProbeOffset(reader, element, 0x108);
    const auto sections_pointer_after = ReadProbeOffset(reader, node.address.value, 0x30);
    const auto materials_pointer_after = ReadProbeOffset(reader, element, 0x104);
    const auto material_count_after = ReadProbeU32(reader, node.instance_materials.value - 4);
    if (!node_after.available || node_after.value != node.address.value ||
        !sections_pointer_after.available || sections_pointer_after.value != node.section_array.value ||
        !materials_pointer_after.available || materials_pointer_after.value != node.instance_materials.value ||
        !material_count_after.available || material_count_after.value != material_count.value ||
        !ReadProbe(reader, node.section_array.value, sections_after, section_bytes) ||
        !ReadProbe(reader, node.instance_materials.value, materials_after, material_bytes) ||
        std::memcmp(sections, sections_after, section_bytes) != 0 ||
        std::memcmp(materials, materials_after, material_bytes) != 0) return;
    node.sections_copied = node.section_count;
    node.sections_available = true;
}

inline void ReadProbeMeshNode(const ProbeReader& reader, std::uint32_t element, ProbeMeshNode& node) noexcept {
    // Static analysis: 0x10dcaf90 stores the named mesh node at element+0x108;
    // the live list and links are verified before any node metadata is used.
    constexpr std::uint32_t kNodePool = 0x10f31bb8;
    struct Pool { std::uint32_t head, tail, count; } pool{}, after{};
    struct Links { std::uint32_t previous, next; } links{}, target_links{}, links_after{};
    node.address = ReadProbeOffset(reader, element, 0x108);
    if (!node.address.available || !node.address.value || !ReadProbe(reader, kNodePool, &pool, sizeof(pool))) return;
    if (pool.count > kMaxMeshNodeListEntries) { node.pool_scan_truncated = true; return; }
    if (!pool.count || !pool.head || !pool.tail) return;
    std::uint32_t current = pool.head, previous = 0;
    for (std::uint32_t i = 0; i < pool.count; ++i) {
        if (!current || !ReadProbeBytesOffset(reader, current, 0xa8, &links, sizeof(links)) ||
            links.previous != previous) return;
        if (current == node.address.value) { ++node.occurrences; target_links = links; }
        ++node.nodes_scanned;
        previous = current;
        current = links.next;
    }
    if (current || previous != pool.tail || !ReadProbe(reader, kNodePool, &after, sizeof(after)) ||
        std::memcmp(&pool, &after, sizeof(pool)) != 0) return;
    node.pool_scan_complete = true;
    if (node.occurrences != 1) return;
    node.vtable = ReadProbeU32(reader, node.address.value);
    node.name_address = ReadProbeOffset(reader, node.address.value, 0xb0);
    if (!node.vtable.available || node.vtable.value != 0x10e49d14 ||
        !node.name_address.available || !node.name_address.value) return;
    std::uint32_t counts[4]{};
    node.counts_available = ReadProbeBytesOffset(reader, node.address.value, 0x04, counts, sizeof(counts));
    node.bounds_available = ReadProbeBytesOffset(reader, node.address.value, 0x74, node.bounds_bits, sizeof(node.bounds_bits));
    node.name_available = ReadMeshNodeName(reader, node.name_address.value, node.name);
    if (!node.counts_available || !node.bounds_available || !node.name_available) return;
    node.vertex_count = counts[0]; node.triangle_count = counts[1];
    node.index_count = counts[2]; node.section_count = counts[3];
    if (node.vertex_count > 65536 || node.triangle_count > 65536 ||
        node.index_count > 65536 || node.section_count > 65536) return;
    float bounds[6]{};
    for (std::size_t i = 0; i < 6; ++i) {
        if ((node.bounds_bits[i] & 0x7f800000u) == 0x7f800000u) return;
        std::memcpy(&bounds[i], &node.bounds_bits[i], sizeof(float));
    }
    for (std::size_t i = 0; i < 3; ++i) if (bounds[i] > bounds[i + 3]) return;
    std::uint32_t counts_after[4]{}, bounds_after[6]{};
    char name_after[128]{};
    const auto element_after = ReadProbeOffset(reader, element, 0x108);
    const auto vtable_after = ReadProbeU32(reader, node.address.value);
    const auto name_address_after = ReadProbeOffset(reader, node.address.value, 0xb0);
    node.stable = element_after.available && element_after.value == node.address.value &&
        vtable_after.available && vtable_after.value == node.vtable.value &&
        name_address_after.available && name_address_after.value == node.name_address.value &&
        ReadProbeBytesOffset(reader, node.address.value, 0x04, counts_after, sizeof(counts_after)) &&
        ReadProbeBytesOffset(reader, node.address.value, 0x74, bounds_after, sizeof(bounds_after)) &&
        ReadMeshNodeName(reader, node.name_address.value, name_after) &&
        std::memcmp(counts, counts_after, sizeof(counts)) == 0 &&
        std::memcmp(node.bounds_bits, bounds_after, sizeof(bounds_after)) == 0 &&
        std::strcmp(node.name, name_after) == 0 &&
        ReadProbeBytesOffset(reader, node.address.value, 0xa8, &links_after, sizeof(links_after)) &&
        std::memcmp(&target_links, &links_after, sizeof(target_links)) == 0 &&
        ReadProbe(reader, kNodePool, &after, sizeof(after)) && std::memcmp(&pool, &after, sizeof(pool)) == 0;
    if (!node.stable) return;
    ReadMeshNodeSections(reader, element, node);
    // Bracket section reads with the same node, pool and metadata checks.
    const auto final_element = ReadProbeOffset(reader, element, 0x108);
    const auto final_vtable = ReadProbeU32(reader, node.address.value);
    const auto final_name_address = ReadProbeOffset(reader, node.address.value, 0xb0);
    node.stable = final_element.available && final_element.value == node.address.value &&
        final_vtable.available && final_vtable.value == node.vtable.value &&
        final_name_address.available && final_name_address.value == node.name_address.value &&
        ReadMeshNodeName(reader, node.name_address.value, name_after) && std::strcmp(node.name, name_after) == 0 &&
        ReadProbeBytesOffset(reader, node.address.value, 0x04, counts_after, sizeof(counts_after)) &&
        ReadProbeBytesOffset(reader, node.address.value, 0x74, bounds_after, sizeof(bounds_after)) &&
        std::memcmp(counts, counts_after, sizeof(counts)) == 0 &&
        std::memcmp(node.bounds_bits, bounds_after, sizeof(bounds_after)) == 0 &&
        ReadProbeBytesOffset(reader, node.address.value, 0xa8, &links_after, sizeof(links_after)) &&
        std::memcmp(&target_links, &links_after, sizeof(target_links)) == 0 &&
        ReadProbe(reader, kNodePool, &after, sizeof(after)) && std::memcmp(&pool, &after, sizeof(pool)) == 0;
    if (!node.stable) node.sections_available = false;
}

inline void ReadProbeBspSections(const ProbeReader& reader, ProbeBspBlock& block) noexcept {
    if (!block.stable) return;
    if (block.section_count > kMaxProbeBspSections) { block.sections_truncated = true; return; }
    if (!block.section_count) { block.sections_available = true; return; }
    block.section_array = ReadProbeOffset(reader, block.address.value, 0x130);
    if (!block.section_array.available || block.section_array.value < 4) return;
    const auto count = ReadProbeU32(reader, block.section_array.value - 4);
    if (!count.available || count.value != block.section_count) return;
    std::uint32_t entries[kMaxProbeBspSections][9]{}, after[kMaxProbeBspSections][9]{};
    const auto size = block.section_count * sizeof(entries[0]);
    if (!ReadProbe(reader, block.section_array.value, entries, size)) return;
    for (std::uint32_t i = 0; i < block.section_count; ++i) {
        auto& out = block.sections[i];
        out.triangles = entries[i][3]; out.first_vertex = entries[i][4]; out.vertex_count = entries[i][5];
        out.first_index = entries[i][6]; out.index_count = entries[i][7]; out.unknown = entries[i][8];
        if (std::uint64_t{out.first_vertex} + out.vertex_count > block.vertex_count ||
            std::uint64_t{out.first_index} + out.index_count > block.index_count ||
            !ReadMeshSectionName(reader, entries[i][0], out.material_name, out.material_name_available)) return;
    }
    const auto array_after = ReadProbeOffset(reader, block.address.value, 0x130);
    const auto count_after = ReadProbeU32(reader, block.section_array.value - 4);
    if (!array_after.available || array_after.value != block.section_array.value ||
        !count_after.available || count_after.value != count.value ||
        !ReadProbe(reader, block.section_array.value, after, size) || std::memcmp(entries, after, size) != 0) return;
    block.sections_copied = block.section_count;
    block.sections_available = true;
}

inline void ReadProbeBsp(const ProbeReader& reader, ProbeSnapshot& snapshot) noexcept {
    // 0x10947fc0 writes the loaded VBSP pointer at renderer+0xc. 0x10947670
    // builds its Class_10E4A668 array at VBSP+0x54, stride 0x1ac.
    constexpr std::uint32_t kRenderer = 0x10f2c740, kMagic = 0x50534256, kStride = 0x1ac;
    auto& bsp = snapshot.bsp;
    bsp.renderer = ReadProbeU32(reader, kRenderer);
    if (!bsp.renderer.available || !bsp.renderer.value) return;
    bsp.source = ReadProbeOffset(reader, bsp.renderer.value, 0x0c);
    if (!bsp.source.available || !bsp.source.value) return;
    const auto magic = ReadProbeU32(reader, bsp.source.value);
    const auto count = ReadProbeOffset(reader, bsp.source.value, 0x0c);
    bsp.blocks = ReadProbeOffset(reader, bsp.source.value, 0x54);
    if (!magic.available || magic.value != kMagic || !count.available || !count.value || count.value > 65536 ||
        !bsp.blocks.available || bsp.blocks.value < 4 ||
        std::uint64_t{bsp.blocks.value} + std::uint64_t{count.value} * kStride > (std::uint64_t{1} << 32)) return;
    const auto allocated_count = ReadProbeU32(reader, bsp.blocks.value - 4);
    if (!allocated_count.available || allocated_count.value != count.value) return;
    bsp.block_count = count.value;
    bsp.candidates_truncated = snapshot.entries_truncated;
    for (std::uint32_t i = 0; i < snapshot.entries_attempted; ++i) {
        const auto& entry = snapshot.entries[i];
        if (!entry.address.available || !entry.address.value || !entry.type.available || entry.type.value != 5 ||
            !entry.vtable.available || entry.vtable.value != 0x10e4a668) continue;
        ++bsp.type5_seen;
        if (entry.address.value < bsp.blocks.value) continue;
        const auto distance = entry.address.value - bsp.blocks.value;
        if (distance % kStride || distance / kStride >= bsp.block_count) continue;
        ++bsp.member_count;
        if (bsp.candidates_copied == kMaxProbeBspBlocks) { bsp.candidates_truncated = true; continue; }
        auto& block = bsp.candidates[bsp.candidates_copied++];
        block.list_index = entry.list_index; block.source_index = distance / kStride; block.address = entry.address;
        std::uint32_t counts[4]{}, counts_after[4]{}, bounds_after[6]{}, transform_after[16]{};
        block.metadata_available = ReadProbeBytesOffset(reader, block.address.value, 0xe4, counts, sizeof(counts)) &&
            ReadProbeBytesOffset(reader, block.address.value, 0x17c, block.bounds_bits, sizeof(block.bounds_bits)) &&
            ReadProbeBytesOffset(reader, block.address.value, 0x04, block.transform_bits, sizeof(block.transform_bits));
        if (!block.metadata_available) continue;
        block.vertex_count = counts[0]; block.triangle_count = counts[1];
        block.index_count = counts[2]; block.section_count = counts[3];
        if (block.vertex_count > 65536 || block.triangle_count > 65536 ||
            block.index_count > 65536 || block.section_count > 65536) continue;
        float bounds[6]{};
        bool finite = true;
        for (std::size_t j = 0; j < 6; ++j) {
            if ((block.bounds_bits[j] & 0x7f800000u) == 0x7f800000u) finite = false;
            std::memcpy(&bounds[j], &block.bounds_bits[j], sizeof(float));
        }
        for (auto bits : block.transform_bits) if ((bits & 0x7f800000u) == 0x7f800000u) finite = false;
        if (!finite || bounds[0] > bounds[3] || bounds[1] > bounds[4] || bounds[2] > bounds[5]) continue;
        const auto vtable_after = ReadProbeU32(reader, block.address.value);
        const auto type_after = ReadProbeOffset(reader, block.address.value, 0x44);
        const auto slot_after = ReadProbeOffset(reader, snapshot.array.value, block.list_index * 4);
        block.stable = vtable_after.available && vtable_after.value == 0x10e4a668 &&
            type_after.available && type_after.value == 5 && slot_after.available && slot_after.value == block.address.value &&
            ReadProbeBytesOffset(reader, block.address.value, 0xe4, counts_after, sizeof(counts_after)) &&
            ReadProbeBytesOffset(reader, block.address.value, 0x17c, bounds_after, sizeof(bounds_after)) &&
            ReadProbeBytesOffset(reader, block.address.value, 0x04, transform_after, sizeof(transform_after)) &&
            std::memcmp(counts, counts_after, sizeof(counts)) == 0 &&
            std::memcmp(block.bounds_bits, bounds_after, sizeof(bounds_after)) == 0 &&
            std::memcmp(block.transform_bits, transform_after, sizeof(transform_after)) == 0;
        if (block.stable) ReadProbeBspSections(reader, block);
        const auto final_slot = ReadProbeOffset(reader, snapshot.array.value, block.list_index * 4);
        block.stable = block.stable && final_slot.available && final_slot.value == block.address.value &&
            ReadProbeBytesOffset(reader, block.address.value, 0xe4, counts_after, sizeof(counts_after)) &&
            ReadProbeBytesOffset(reader, block.address.value, 0x17c, bounds_after, sizeof(bounds_after)) &&
            ReadProbeBytesOffset(reader, block.address.value, 0x04, transform_after, sizeof(transform_after)) &&
            std::memcmp(counts, counts_after, sizeof(counts)) == 0 &&
            std::memcmp(block.bounds_bits, bounds_after, sizeof(bounds_after)) == 0 &&
            std::memcmp(block.transform_bits, transform_after, sizeof(transform_after)) == 0;
        if (!block.stable) block.sections_available = false;
    }
    const auto renderer_after = ReadProbeU32(reader, kRenderer);
    const auto source_after = ReadProbeOffset(reader, bsp.renderer.value, 0x0c);
    const auto magic_after = ReadProbeU32(reader, bsp.source.value);
    const auto count_after = ReadProbeOffset(reader, bsp.source.value, 0x0c);
    const auto blocks_after = ReadProbeOffset(reader, bsp.source.value, 0x54);
    const auto allocated_after = ReadProbeU32(reader, bsp.blocks.value - 4);
    const auto list_count_after = ReadProbeOffset(reader, snapshot.self, 0x50);
    const auto list_after = ReadProbeOffset(reader, snapshot.self, 0x58);
    bsp.source_stable = renderer_after.available && renderer_after.value == bsp.renderer.value &&
        source_after.available && source_after.value == bsp.source.value && magic_after.available && magic_after.value == kMagic &&
        count_after.available && count_after.value == count.value && blocks_after.available && blocks_after.value == bsp.blocks.value &&
        allocated_after.available && allocated_after.value == allocated_count.value &&
        list_count_after.available && list_count_after.value == snapshot.count.value &&
        list_after.available && list_after.value == snapshot.array.value;
    if (!bsp.source_stable) for (std::size_t i = 0; i < bsp.candidates_copied; ++i) {
        bsp.candidates[i].stable = false;
        bsp.candidates[i].sections_available = false;
    }
}

inline void ReadProbeCamera(const ProbeReader& reader, std::uint32_t global, ProbeCamera& camera) noexcept {
    camera.address = ReadProbeU32(reader, global);
    if (!camera.address.available || !camera.address.value) return;
    std::uint32_t address = 0;
    camera.source_matrix_available = ProbeOffset(camera.address.value, 0x04, address) &&
        ReadProbe(reader, address, camera.source_matrix_bits, sizeof(camera.source_matrix_bits));
    camera.projection_available = ProbeOffset(camera.address.value, 0x64, address) &&
        ReadProbe(reader, address, camera.projection_bits, sizeof(camera.projection_bits));
    camera.derived_matrix_available = ProbeOffset(camera.address.value, 0xe4, address) &&
        ReadProbe(reader, address, camera.derived_matrix_bits, sizeof(camera.derived_matrix_bits));
    camera.combined_matrix_available = ProbeOffset(camera.address.value, 0x124, address) &&
        ReadProbe(reader, address, camera.combined_matrix_bits, sizeof(camera.combined_matrix_bits));
    camera.viewport_available = ProbeOffset(camera.address.value, 0x164, address) &&
        ReadProbe(reader, address, camera.viewport_bits, sizeof(camera.viewport_bits));
    std::uint32_t input[16]{}, projection[16]{}, derived[16]{}, combined[16]{}, viewport[6]{};
    const ProbeValue current = ReadProbeU32(reader, global);
    camera.stable = camera.source_matrix_available && camera.projection_available && camera.derived_matrix_available &&
        camera.combined_matrix_available && camera.viewport_available &&
        current.available && current.value == camera.address.value &&
        ReadProbeBytesOffset(reader, camera.address.value, 0x04, input, sizeof(input)) &&
        ReadProbeBytesOffset(reader, camera.address.value, 0x64, projection, sizeof(projection)) &&
        ReadProbeBytesOffset(reader, camera.address.value, 0xe4, derived, sizeof(derived)) &&
        ReadProbeBytesOffset(reader, camera.address.value, 0x124, combined, sizeof(combined)) &&
        ReadProbeBytesOffset(reader, camera.address.value, 0x164, viewport, sizeof(viewport)) &&
        std::memcmp(input, camera.source_matrix_bits, sizeof(input)) == 0 &&
        std::memcmp(projection, camera.projection_bits, sizeof(projection)) == 0 &&
        std::memcmp(derived, camera.derived_matrix_bits, sizeof(derived)) == 0 &&
        std::memcmp(combined, camera.combined_matrix_bits, sizeof(combined)) == 0 &&
        std::memcmp(viewport, camera.viewport_bits, sizeof(viewport)) == 0;
}

inline void RecheckProbeCamera(const ProbeReader& reader, std::uint32_t global, ProbeCamera& original) noexcept {
    ProbeCamera later;
    ReadProbeCamera(reader, global, later);
    original.stable = original.stable && later.stable && original.address.available && later.address.available &&
        original.address.value == later.address.value &&
        std::memcmp(original.source_matrix_bits, later.source_matrix_bits, sizeof(original.source_matrix_bits)) == 0 &&
        std::memcmp(original.projection_bits, later.projection_bits, sizeof(original.projection_bits)) == 0 &&
        std::memcmp(original.derived_matrix_bits, later.derived_matrix_bits, sizeof(original.derived_matrix_bits)) == 0 &&
        std::memcmp(original.combined_matrix_bits, later.combined_matrix_bits, sizeof(original.combined_matrix_bits)) == 0 &&
        std::memcmp(original.viewport_bits, later.viewport_bits, sizeof(original.viewport_bits)) == 0;
}

struct ProbeArray32 { std::uint32_t data, count, capacity; };

inline bool ValidProbeArray(const ProbeArray32& array, std::uint32_t maximum) noexcept {
    return array.count <= array.capacity && array.count <= maximum &&
           array.capacity <= 4 * maximum && (!array.count || array.data != 0);
}

inline bool SameProbeArray(const ProbeArray32& a, const ProbeArray32& b) noexcept {
    return a.data == b.data && a.count == b.count && a.capacity == b.capacity;
}

inline bool ReadProbeName(const ProbeReader& reader, std::uint32_t fname, char (&name)[64]) noexcept {
    // FName::Names contains FNameEntry pointers; +0x1c holds ANSI base text.
    ProbeArray32 names{}, after{};
    constexpr std::uint32_t kNames = 0x10f7af1c;
    if (!ReadProbe(reader, kNames, &names, sizeof(names)) || !ValidProbeArray(names, 65536)) return false;
    const std::uint32_t index = fname & 0xffff;
    if (index >= names.count) return false;
    std::uint32_t slot = 0, text = 0;
    if (!ProbeOffset(names.data, index * 4, slot)) return false;
    const ProbeValue entry = ReadProbeU32(reader, slot);
    if (!entry.available || !ProbeOffset(entry.value, 0x1c, text)) return false;
    char copied[64]{};
    if (!ReadProbe(reader, text, copied, sizeof(copied))) return false;
    std::size_t length = 0;
    for (; length < sizeof(copied) && copied[length]; ++length) {
        const auto c = static_cast<unsigned char>(copied[length]);
        if (c < 0x20 || c > 0x7e) return false;
    }
    if (!length || length == sizeof(copied)) return false;
    char verify[64]{};
    if (!ReadProbe(reader, text, verify, sizeof(verify)) || std::memcmp(copied, verify, length + 1) != 0 ||
        !ReadProbe(reader, kNames, &after, sizeof(after)) || !SameProbeArray(names, after)) return false;
    const ProbeValue again = ReadProbeU32(reader, slot);
    if (!again.available || again.value != entry.value) return false;
    std::memcpy(name, copied, length + 1);
    return true;
}

inline void ReadProbeOwners(const ProbeReader& reader, ProbeSnapshot& snapshot) noexcept {
    constexpr std::uint32_t kObjects = 0x10f3e4a0;
    constexpr std::uint32_t kMaximumObjects = 65536;
    constexpr std::uint32_t kMissing = std::numeric_limits<std::uint32_t>::max();
    constexpr std::uint32_t kDuplicate = kMissing - 1;
    bool any = false;
    for (std::uint32_t i = 0; i < snapshot.entries_attempted; ++i) {
        auto& item = snapshot.entries[i];
        if (item.address.available && item.vtable.available && item.vtable.value == 0x10e4e8c0 &&
            item.type.available && item.type.value == 4) {
            item.owner_candidate = ReadProbeOffset(reader, item.address.value, 0x54);
            any = any || (item.owner_candidate.available && item.owner_candidate.value);
        }
    }
    if (!any) return;
    ProbeArray32 table{}, after{};
    if (!ReadProbe(reader, kObjects, &table, sizeof(table)) || !ValidProbeArray(table, kMaximumObjects)) return;
    std::uint32_t slots[kMaxProbeEntries];
    for (auto& slot : slots) slot = kMissing;
    struct Candidate { std::uint32_t owner, index; } candidates[kMaxProbeEntries];
    std::size_t candidate_count = 0;
    for (std::uint32_t i = 0; i < snapshot.entries_attempted; ++i) {
        const auto& owner = snapshot.entries[i].owner_candidate;
        if (owner.available && owner.value) candidates[candidate_count++] = {owner.value, i};
    }
    std::sort(candidates, candidates + candidate_count, [](const Candidate& a, const Candidate& b) {
        return a.owner < b.owner;
    });
    // Scan each object-table slot once, using bounded chunks. Never dereference
    // a candidate owner until membership in a stable complete table is proven.
    for (std::uint32_t start = 0; start < table.count;) {
        std::uint32_t values[128]{};
        const auto count = std::min<std::uint32_t>(128, table.count - start);
        std::uint32_t address = 0;
        if (!ProbeOffset(table.data, start * 4, address) || !ReadProbe(reader, address, values, count * 4)) return;
        for (std::uint32_t j = 0; j < count; ++j) {
            if (!values[j]) continue;
            auto* match = std::lower_bound(candidates, candidates + candidate_count, values[j],
                [](const Candidate& candidate, std::uint32_t value) { return candidate.owner < value; });
            for (; match != candidates + candidate_count && match->owner == values[j]; ++match) {
                auto& slot = slots[match->index];
                slot = slot == kMissing ? start + j : kDuplicate;
            }
        }
        start += count;
    }
    if (!ReadProbe(reader, kObjects, &after, sizeof(after)) || !SameProbeArray(table, after)) return;
    snapshot.object_table_complete = true;
    for (std::uint32_t i = 0; i < snapshot.entries_attempted; ++i) {
        auto& item = snapshot.entries[i];
        if (slots[i] >= kDuplicate) continue;
        std::uint32_t slot_address = 0;
        if (!ProbeOffset(table.data, slots[i] * 4, slot_address)) continue;
        const ProbeValue slot = ReadProbeU32(reader, slot_address);
        if (!slot.available || slot.value != item.owner_candidate.value) continue;
        item.owner_table_member = true;
        const std::uint32_t owner = item.owner_candidate.value;
        item.owner_object_index = ReadProbeOffset(reader, owner, 0x04);
        item.owner_fname = ReadProbeOffset(reader, owner, 0x20);
        item.owner_class = ReadProbeOffset(reader, owner, 0x24);
        item.owner_backlink = ReadProbeOffset(reader, owner, 0xb4);
        item.owner_location_available = ReadProbeBytesOffset(reader, owner, 0x2c,
            item.owner_location_bits, sizeof(item.owner_location_bits));
        item.owner_rotation_available = ReadProbeBytesOffset(reader, owner, 0x38,
            item.owner_rotation_bits, sizeof(item.owner_rotation_bits));
        item.object_matrix_available = ReadProbeBytesOffset(reader, item.address.value, 0x04,
            item.object_matrix_bits, sizeof(item.object_matrix_bits));
        if (item.owner_fname.available) item.owner_name_available = ReadProbeName(reader, item.owner_fname.value, item.owner_name);
        std::uint32_t location[3]{}, rotation[3]{}, matrix[16]{};
        const ProbeValue check_slot = ReadProbeU32(reader, slot_address);
        const ProbeValue check_index = ReadProbeOffset(reader, owner, 0x04);
        const ProbeValue check_name = ReadProbeOffset(reader, owner, 0x20);
        const ProbeValue check_class = ReadProbeOffset(reader, owner, 0x24);
        const ProbeValue check_back = ReadProbeOffset(reader, owner, 0xb4);
        const ProbeValue check_vtable = ReadProbeU32(reader, item.address.value);
        const ProbeValue check_type = ReadProbeOffset(reader, item.address.value, 0x44);
        const ProbeValue check_owner = ReadProbeOffset(reader, item.address.value, 0x54);
        const ProbeValue check_count = ReadProbeOffset(reader, snapshot.self, 0x50);
        const ProbeValue check_array = ReadProbeOffset(reader, snapshot.self, 0x58);
        std::uint32_t entry_address = 0;
        const ProbeValue check_entry = ProbeOffset(snapshot.array.value, item.list_index * 4, entry_address) ?
            ReadProbeU32(reader, entry_address) : ProbeValue{};
        item.owner_stable = item.owner_object_index.available && item.owner_object_index.value == slots[i] &&
            item.owner_fname.available && item.owner_class.available && item.owner_backlink.available &&
            item.owner_backlink.value == item.address.value && item.owner_location_available &&
            item.owner_rotation_available && item.object_matrix_available &&
            check_slot.available && check_slot.value == owner && check_index.available && check_index.value == slots[i] &&
            check_name.available && check_name.value == item.owner_fname.value &&
            check_class.available && check_class.value == item.owner_class.value &&
            check_back.available && check_back.value == item.address.value &&
            check_vtable.available && check_vtable.value == item.vtable.value &&
            check_type.available && check_type.value == item.type.value && check_owner.available && check_owner.value == owner &&
            check_count.available && check_count.value == snapshot.count.value &&
            check_array.available && check_array.value == snapshot.array.value &&
            check_entry.available && check_entry.value == item.address.value &&
            ReadProbeBytesOffset(reader, owner, 0x2c, location, sizeof(location)) &&
            ReadProbeBytesOffset(reader, owner, 0x38, rotation, sizeof(rotation)) &&
            ReadProbeBytesOffset(reader, item.address.value, 0x04, matrix, sizeof(matrix)) &&
            std::memcmp(location, item.owner_location_bits, sizeof(location)) == 0 &&
            std::memcmp(rotation, item.owner_rotation_bits, sizeof(rotation)) == 0 &&
            std::memcmp(matrix, item.object_matrix_bits, sizeof(matrix)) == 0 &&
            ReadProbe(reader, kObjects, &after, sizeof(after)) && SameProbeArray(table, after);
    }
}

inline bool FindUniqueProbeObject(const ProbeReader& reader, const ProbeArray32& table,
                                  std::uint32_t wanted, std::uint32_t& slot) noexcept {
    if (!wanted) return false;
    unsigned found = 0;
    for (std::uint32_t start = 0; start < table.count;) {
        std::uint32_t values[128]{};
        const auto count = std::min<std::uint32_t>(128, table.count - start);
        std::uint32_t address = 0;
        if (!ProbeOffset(table.data, start * 4, address) || !ReadProbe(reader, address, values, count * 4)) return false;
        for (std::uint32_t i = 0; i < count; ++i) if (values[i] == wanted) { slot = start + i; ++found; }
        start += count;
    }
    ProbeArray32 after{};
    return found == 1 && ReadProbe(reader, 0x10f3e4a0, &after, sizeof(after)) && SameProbeArray(table, after);
}

inline ProbeNamedObject ReadNamedProbeObject(const ProbeReader& reader, const ProbeArray32& table,
                                             ProbeValue pointer) noexcept {
    ProbeNamedObject result;
    result.address = pointer;
    std::uint32_t slot = 0, slot_address = 0;
    if (!pointer.available || !FindUniqueProbeObject(reader, table, pointer.value, slot) ||
        !ProbeOffset(table.data, slot * 4, slot_address)) return result;
    const auto member = ReadProbeU32(reader, slot_address);
    if (!member.available || member.value != pointer.value) return result;
    result.member = true;
    result.index = ReadProbeOffset(reader, pointer.value, 0x04);
    result.fname = ReadProbeOffset(reader, pointer.value, 0x20);
    result.outer = ReadProbeOffset(reader, pointer.value, 0x18);
    if (result.fname.available) result.name_available = ReadProbeName(reader, result.fname.value, result.name);
    const auto check_member = ReadProbeU32(reader, slot_address);
    const auto check_index = ReadProbeOffset(reader, pointer.value, 0x04);
    const auto check_name = ReadProbeOffset(reader, pointer.value, 0x20);
    const auto check_outer = ReadProbeOffset(reader, pointer.value, 0x18);
    ProbeArray32 after{};
    result.stable = result.index.available && result.index.value == slot && result.fname.available &&
        result.outer.available && check_member.available && check_member.value == pointer.value &&
        check_index.available && check_index.value == slot && check_name.available &&
        check_name.value == result.fname.value && check_outer.available && check_outer.value == result.outer.value &&
        ReadProbe(reader, 0x10f3e4a0, &after, sizeof(after)) && SameProbeArray(table, after);
    return result;
}

inline void ReadTargetProbe(const ProbeReader& reader, ProbeSnapshot& snapshot) noexcept {
    auto& target = snapshot.target;
    ProbeArray32 table{}, after{};
    if (!ReadProbe(reader, 0x10f3e4a0, &table, sizeof(table))) return;
    target.object_scan_truncated = table.count > kMaxTargetObjects;
    if (!ValidProbeArray(table, static_cast<std::uint32_t>(kMaxTargetObjects))) return;
    bool complete = true;
    std::uint32_t actor = 0, actor_slot = 0;
    for (std::uint32_t start = 0; start < table.count;) {
        std::uint32_t values[128]{};
        const auto count = std::min<std::uint32_t>(128, table.count - start);
        std::uint32_t address = 0;
        if (!ProbeOffset(table.data, start * 4, address) || !ReadProbe(reader, address, values, count * 4)) {
            complete = false; break;
        }
        for (std::uint32_t j = 0; j < count; ++j) {
            ++target.objects_scanned;
            if (!values[j]) continue;
            // Every pointer is first observed as a GObjObjects member. Only
            // the generic UObject FName field is read before identifying it.
            const auto fname = ReadProbeOffset(reader, values[j], 0x20);
            if (!fname.available) { complete = false; continue; }
            if ((fname.value >> 16) != 1521) continue;
            char name[64]{};
            if (!ReadProbeName(reader, fname.value, name)) { complete = false; continue; }
            if (std::strcmp(name, "StaticMeshActor") == 0) {
                ++target.actors_named_target;
                if (target.actors_named_target == 1) { actor = values[j]; actor_slot = start + j; }
            }
        }
        start += count;
    }
    if (!ReadProbe(reader, 0x10f3e4a0, &after, sizeof(after)) || !SameProbeArray(table, after)) complete = false;
    target.object_scan_complete = complete && target.objects_scanned == table.count;
    if (target.actors_named_target != 1 || !actor || !snapshot.count.available || !snapshot.array.available ||
        (snapshot.count.value && !snapshot.array.value)) return;
    const auto actor_index = ReadProbeOffset(reader, actor, 0x04);
    const auto flesh = ReadProbeOffset(reader, actor, 0xb4);
    std::uint32_t object_slot = 0;
    if (!actor_index.available || actor_index.value != actor_slot || !flesh.available || !flesh.value ||
        !ProbeOffset(table.data, actor_slot * 4, object_slot)) return;
    const auto actor_again = ReadProbeU32(reader, object_slot);
    if (!actor_again.available || actor_again.value != actor) return;
    const auto limit = std::min<std::uint32_t>(snapshot.count.value, static_cast<std::uint32_t>(kMaxTargetListEntries));
    target.list_scan_truncated = snapshot.count.value > kMaxTargetListEntries;
    std::uint32_t match_index = 0;
    bool list_ok = true;
    for (std::uint32_t start = 0; start < limit;) {
        std::uint32_t values[128]{};
        const auto count = std::min<std::uint32_t>(128, limit - start);
        std::uint32_t address = 0;
        if (!ProbeOffset(snapshot.array.value, start * 4, address) || !ReadProbe(reader, address, values, count * 4)) {
            list_ok = false; break;
        }
        for (std::uint32_t j = 0; j < count; ++j) if (values[j] == flesh.value) {
            ++target.list_occurrences;
            if (target.list_occurrences == 1) match_index = start + j;
        }
        target.list_scanned += count;
        start += count;
    }
    const auto count_after = ReadProbeOffset(reader, snapshot.self, 0x50);
    const auto array_after = ReadProbeOffset(reader, snapshot.self, 0x58);
    target.list_scan_complete = list_ok && !target.list_scan_truncated && target.list_scanned == snapshot.count.value &&
        count_after.available && count_after.value == snapshot.count.value &&
        array_after.available && array_after.value == snapshot.array.value;
    if (target.list_occurrences != 1) return;
    // Reuse the same guarded membership/backlink checks for the matched item,
    // even when its actual list index lies beyond the generic 128-item prefix.
    ProbeSnapshot single;
    single.self = snapshot.self;
    single.count = snapshot.count;
    single.array = snapshot.array;
    single.entries_attempted = 1;
    auto& item = single.entries[0];
    item.list_index = match_index;
    item.address = {flesh.value, true};
    item.vtable = ReadProbeU32(reader, flesh.value);
    item.type = ReadProbeOffset(reader, flesh.value, 0x44);
    std::uint32_t flags_address = 0;
    std::uint8_t flags = 0;
    if (ProbeOffset(flesh.value, 0x48, flags_address) && ReadProbe(reader, flags_address, &flags, 1))
        item.flags = {flags, true};
    ReadProbeOwners(reader, single);
    if (!item.owner_table_member || !item.owner_candidate.available || item.owner_candidate.value != actor) return;
    target.entry = item;
    target.entry_available = true;
    target.owner_class = ReadNamedProbeObject(reader, table, item.owner_class);
    auto outer = ReadProbeOffset(reader, actor, 0x18);
    while (outer.available && outer.value && target.outer_count < 4) {
        auto& named = target.outer[target.outer_count++];
        named = ReadNamedProbeObject(reader, table, outer);
        if (!named.member || !named.stable) break;
        outer = named.outer;
    }
    target.outer_truncated = outer.available && outer.value && target.outer_count == 4;
    if (target.object_scan_complete && target.list_scan_complete && target.actors_named_target == 1 &&
        target.list_occurrences == 1 && target.entry.owner_stable && target.owner_class.member &&
        target.owner_class.stable && target.owner_class.name_available &&
        target.owner_class.fname.available && (target.owner_class.fname.value >> 16) == 0 &&
        std::strcmp(target.owner_class.name, "StaticMeshActor") == 0) {
        const AssetReader asset_reader{reader.context, reader.read};
        target.asset = InspectAssetCandidate(asset_reader, actor);
        ReadProbeMeshNode(reader, flesh.value, target.mesh_node);
        // The asset reader copies a bounded candidate; bracket it with a fresh
        // list/owner validation so the property is not attached to a changed
        // element or actor. This cannot eliminate ABA or unobserved writes.
        const ProbeEntry before = target.entry;
        single.entries[0] = {};
        single.entries[0].list_index = match_index;
        single.entries[0].address = {flesh.value, true};
        single.entries[0].vtable = ReadProbeU32(reader, flesh.value);
        single.entries[0].type = ReadProbeOffset(reader, flesh.value, 0x44);
        ReadProbeOwners(reader, single);
        const auto& reread = single.entries[0];
        if (!reread.owner_stable || !reread.owner_candidate.available || reread.owner_candidate.value != actor ||
            !reread.owner_fname.available || reread.owner_fname.value != before.owner_fname.value ||
            !reread.owner_class.available || reread.owner_class.value != before.owner_class.value ||
            std::memcmp(reread.owner_location_bits, before.owner_location_bits, sizeof(before.owner_location_bits)) != 0 ||
            std::memcmp(reread.owner_rotation_bits, before.owner_rotation_bits, sizeof(before.owner_rotation_bits)) != 0 ||
            std::memcmp(reread.object_matrix_bits, before.object_matrix_bits, sizeof(before.object_matrix_bits)) != 0)
        {
            target.asset = {};
            target.mesh_node = {};
        }
    }
}

// Live path: identify the bed from the complete current visible list, then
// verify its owner, class, outer, property and mesh node as before. Unlike the
// full diagnostic path, this does not claim global uniqueness across every
// UObject. No object pointer is reused from an earlier frame.
inline void ReadLiveTargetProbe(const ProbeReader& reader, ProbeSnapshot& snapshot) noexcept {
    auto& target=snapshot.target;target.visible_list_only=true;
    if (!snapshot.count.available || !snapshot.array.available || !snapshot.array.value ||
        snapshot.entries_truncated || snapshot.count.value!=snapshot.entries_attempted ||
        snapshot.entries_attempted>kMaxProbeEntries) { target.list_scan_truncated=snapshot.entries_truncated;return; }
    std::uint32_t selected=0, matches=0;
    for (std::uint32_t i=0;i<snapshot.entries_attempted;++i) {
        const auto& entry=snapshot.entries[i];
        if (!entry.address.available) return;
        if (!entry.address.value) continue;
        if (!entry.vtable.available || !entry.type.available) return;
        if (entry.vtable.value!=0x10e4e8c0 || entry.type.value!=4) continue;
        const auto owner=ReadProbeOffset(reader,entry.address.value,0x54);
        if (!owner.available || !owner.value) return;
        const auto fname=ReadProbeOffset(reader,owner.value,0x20);
        if (!fname.available) return;
        if ((fname.value>>16)!=1521) continue;
        char name[64]{};
        if (!ReadProbeName(reader,fname.value,name)) return;
        if (std::strcmp(name,"StaticMeshActor")==0) { selected=i;++matches; }
    }
    std::uint32_t slots[kMaxProbeEntries]{};
    if (snapshot.entries_attempted && !ReadProbe(reader,snapshot.array.value,slots,snapshot.entries_attempted*4)) return;
    for (std::uint32_t i=0;i<snapshot.entries_attempted;++i)
        if (slots[i]!=snapshot.entries[i].address.value) return;
    const auto count=ReadProbeOffset(reader,snapshot.self,0x50), array=ReadProbeOffset(reader,snapshot.self,0x58);
    if (!count.available || count.value!=snapshot.count.value || !array.available || array.value!=snapshot.array.value) return;
    target.list_scanned=snapshot.entries_attempted;target.list_scan_complete=true;target.list_occurrences=matches;
    if (matches!=1) return;
    ProbeSnapshot single;single.self=snapshot.self;single.count=snapshot.count;single.array=snapshot.array;
    single.entries_attempted=1;single.entries[0]=snapshot.entries[selected];
    ReadProbeOwners(reader,single);
    const auto& item=single.entries[0];
    if (!single.object_table_complete || !item.owner_stable || !item.owner_candidate.available ||
        !item.owner_fname.available || (item.owner_fname.value>>16)!=1521 || !item.owner_name_available ||
        std::strcmp(item.owner_name,"StaticMeshActor")!=0) return;
    snapshot.object_table_complete=true;target.entry=item;target.entry_available=true;
    ProbeArray32 table{};
    if (!ReadProbe(reader,0x10f3e4a0,&table,sizeof(table)) ||
        !ValidProbeArray(table,static_cast<std::uint32_t>(kMaxTargetObjects))) return;
    target.owner_class=ReadNamedProbeObject(reader,table,item.owner_class);
    auto outer=ReadProbeOffset(reader,item.owner_candidate.value,0x18);
    while (outer.available && outer.value && target.outer_count<4) {
        auto& named=target.outer[target.outer_count++];named=ReadNamedProbeObject(reader,table,outer);
        if (!named.member || !named.stable) break;
        outer=named.outer;
    }
    target.outer_truncated=outer.available && outer.value && target.outer_count==4;
    if (!target.owner_class.member || !target.owner_class.stable || !target.owner_class.name_available ||
        !target.owner_class.fname.available || (target.owner_class.fname.value>>16)!=0 ||
        std::strcmp(target.owner_class.name,"StaticMeshActor")!=0) return;
    const AssetReader asset_reader{reader.context,reader.read};
    target.asset=InspectAssetCandidate(asset_reader,item.owner_candidate.value);
    ReadProbeMeshNode(reader,item.address.value,target.mesh_node);
    const ProbeEntry before=item;
    single.entries[0]={};single.entries[0].list_index=selected;
    single.entries[0].address=before.address;
    single.entries[0].vtable=ReadProbeU32(reader,before.address.value);
    single.entries[0].type=ReadProbeOffset(reader,before.address.value,0x44);
    ReadProbeOwners(reader,single);
    const auto& later=single.entries[0];
    if (!later.owner_stable || !later.owner_candidate.available || later.owner_candidate.value!=before.owner_candidate.value ||
        !later.owner_fname.available || later.owner_fname.value!=before.owner_fname.value ||
        !later.owner_class.available || later.owner_class.value!=before.owner_class.value ||
        std::memcmp(later.owner_location_bits,before.owner_location_bits,sizeof(before.owner_location_bits)) ||
        std::memcmp(later.owner_rotation_bits,before.owner_rotation_bits,sizeof(before.owner_rotation_bits)) ||
        std::memcmp(later.object_matrix_bits,before.object_matrix_bits,sizeof(before.object_matrix_bits))) {
        target.asset={};target.mesh_node={};
    }
}

inline ProbeSnapshot SampleProbe(const ProbeReader& reader, std::uint32_t thread_id,
                                 std::uint32_t self, std::uint32_t caller) noexcept {
    ProbeSnapshot snapshot;
    snapshot.thread_id = thread_id;
    snapshot.self = self;
    snapshot.caller = caller;
    snapshot.known_caller = caller == 0x109271ae;
    snapshot.pass = ReadProbeU32(reader, 0x10efef54);
    ReadProbeCamera(reader, 0x10f2c738, snapshot.configured_camera);
    ReadProbeCamera(reader, 0x10f2c8c0, snapshot.current_camera);
    ReadProbeCamera(reader, 0x10f2c8c4, snapshot.culling_camera);
    snapshot.count = ReadProbeOffset(reader, self, 0x50);
    snapshot.array = ReadProbeOffset(reader, self, 0x58);
    if (!snapshot.count.available || !snapshot.array.available ||
        !snapshot.array.value || !snapshot.count.value) return snapshot;
    snapshot.entries_attempted = static_cast<std::uint32_t>(
        std::min<std::size_t>(snapshot.count.value, kMaxProbeEntries));
    snapshot.entries_truncated = snapshot.count.value > kMaxProbeEntries;
    for (std::uint32_t i = 0; i < snapshot.entries_attempted; ++i) {
        auto& entry = snapshot.entries[i];
        entry.list_index = i;
        std::uint32_t address = 0;
        if (!ProbeOffset(snapshot.array.value, i * 4, address)) continue;
        entry.address = ReadProbeU32(reader, address);
        if (!entry.address.available || !entry.address.value) continue;
        entry.vtable = ReadProbeU32(reader, entry.address.value);
        entry.type = ReadProbeOffset(reader, entry.address.value, 0x44);
        if (ProbeOffset(entry.address.value, 0x48, address)) {
            std::uint8_t flags = 0;
            if (ReadProbe(reader, address, &flags, sizeof(flags))) {
                entry.flags.value = flags;
                entry.flags.available = true;
            }
        }
    }
    ReadProbeOwners(reader, snapshot);
    ReadTargetProbe(reader, snapshot);
    ReadProbeBsp(reader, snapshot);
    const ProbeValue final_pass = ReadProbeU32(reader, 0x10efef54);
    snapshot.pass_stable = snapshot.pass.available && final_pass.available && snapshot.pass.value == final_pass.value;
    RecheckProbeCamera(reader, 0x10f2c738, snapshot.configured_camera);
    RecheckProbeCamera(reader, 0x10f2c8c0, snapshot.current_camera);
    RecheckProbeCamera(reader, 0x10f2c8c4, snapshot.culling_camera);
    return snapshot;
}

inline ProbeSnapshot SampleLiveProbe(const ProbeReader& reader, std::uint32_t thread_id,
                                     std::uint32_t self, std::uint32_t caller) noexcept {
    ProbeSnapshot snapshot;snapshot.thread_id=thread_id;snapshot.self=self;snapshot.caller=caller;
    snapshot.known_caller=caller==0x109271ae;snapshot.pass=ReadProbeU32(reader,0x10efef54);
    if (snapshot.pass.available && snapshot.pass.value!=0) {
        const auto after=ReadProbeU32(reader,0x10efef54);
        snapshot.pass_stable=after.available && after.value==snapshot.pass.value;
        return snapshot;
    }
    ReadProbeCamera(reader,0x10f2c8c0,snapshot.current_camera);
    snapshot.count=ReadProbeOffset(reader,self,0x50);snapshot.array=ReadProbeOffset(reader,self,0x58);
    if (!snapshot.count.available || !snapshot.array.available || !snapshot.array.value || !snapshot.count.value) return snapshot;
    snapshot.entries_attempted=static_cast<std::uint32_t>(std::min<std::size_t>(snapshot.count.value,kMaxProbeEntries));
    snapshot.entries_truncated=snapshot.count.value>kMaxProbeEntries;
    for (std::uint32_t i=0;i<snapshot.entries_attempted;++i) {
        auto& entry=snapshot.entries[i];entry.list_index=i;
        entry.address=ReadProbeOffset(reader,snapshot.array.value,i*4);
        if (!entry.address.available || !entry.address.value) continue;
        entry.vtable=ReadProbeU32(reader,entry.address.value);
        entry.type=ReadProbeOffset(reader,entry.address.value,0x44);
    }
    ReadLiveTargetProbe(reader,snapshot);
    ReadProbeBsp(reader,snapshot);
    const auto final_pass=ReadProbeU32(reader,0x10efef54);
    snapshot.pass_stable=snapshot.pass.available && final_pass.available && snapshot.pass.value==final_pass.value;
    RecheckProbeCamera(reader,0x10f2c8c0,snapshot.current_camera);
    return snapshot;
}

}  // namespace t3render::scene_bridge
