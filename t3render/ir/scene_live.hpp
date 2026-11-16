#pragma once

// In-process semantic residency for a partial SceneIR frame. This is a value
// model, not a producer of synchronization evidence or a graphics backend.
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace t3render::live {

using Matrix = std::array<float, 16>; // row-major, row vectors, translation 12..14
using Vec2 = std::array<float, 2>;
using Vec3 = std::array<float, 3>;

struct Material {
    std::string id, name;
};
struct Section {
    std::string name;
    std::uint32_t first_index = 0, index_count = 0;
    std::optional<std::string> material_id;
};
enum class ResourceKind { StaticMesh, BspBlock };
struct Geometry {
    std::vector<Vec3> positions, normals, tangents;
    std::vector<Vec2> uv0;
    std::vector<std::uint32_t> indices;
    std::vector<Section> sections;
};
struct Resource {
    std::string id, name;
    ResourceKind kind = ResourceKind::StaticMesh;
    // Fingerprint of the asset source, supplied by the producer, not GPU data.
    std::string source_sha256;
    Geometry geometry;
};
struct Camera {
    Matrix view{}, projection{};
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
    float min_depth = 0, max_depth = 1;
};
// Producer's binding assessment; Candidate must not be promoted to Verified.
enum class BindingEvidence { Unknown, Candidate, Verified };
enum class IdentityScope { Unknown, VisibleList, FullObjectScan };
struct MeshSelection {
    std::string instance_id, resource_id;
    // Optional in-memory attestation from a matcher; never serialized.
    std::shared_ptr<const Resource> matched_resource;
    std::vector<std::optional<std::string>> matched_material_names;
    // Assigned by the producer, never a process address. Changes on identity reuse.
    std::uint64_t generation = 0;
    BindingEvidence binding = BindingEvidence::Unknown;
    IdentityScope identity_scope = IdentityScope::Unknown;
    Matrix transform{};
    // Empty means use section materials; null entries mean unknown, not defaults.
    std::vector<std::optional<std::string>> material_ids;
    std::vector<std::uint32_t> sections;
};
struct BspSelection {
    std::string resource_id;
    std::shared_ptr<const Resource> matched_resource; // in-memory only
    std::vector<std::optional<std::string>> matched_material_names;
    BindingEvidence binding = BindingEvidence::Unknown;
    IdentityScope identity_scope = IdentityScope::Unknown;
    std::vector<std::uint32_t> sections;
};
enum class LengthUnit { Unknown, T3Unit, Meter };
enum class Axes { Unknown, T3Xyz };
enum class UpAxis { Unknown, Z };
enum class Handedness { Unknown, Left, Right };
enum class Winding { Unknown, NormalAligned, NormalOpposed };
enum class UvOrigin { Unknown, TopLeft, BottomLeft };
enum class ClipDepth { Unknown, ZeroToOne, MinusOneToOne };
struct Conventions {
    LengthUnit length_unit = LengthUnit::Unknown;
    std::optional<float> meters_per_unit;
    Axes axes = Axes::Unknown;
    UpAxis up_axis = UpAxis::Unknown;
    Handedness handedness = Handedness::Unknown;
    Winding winding = Winding::Unknown;
    UvOrigin uv_origin = UvOrigin::Unknown;
    ClipDepth clip_depth = ClipDepth::Unknown;
    // Matrix layout/vectors are always row-major/row; tangent space is unknown.
};
enum class FrameOrigin { Unknown, LiveProcess, CapturedReplay, Synthetic };
struct FrameProvenance {
    FrameOrigin origin = FrameOrigin::Unknown;
    // Assigned per producer session; never an address or OS handle.
    std::string session_id;
    // Required for CapturedReplay; absent for live and synthetic sources.
    std::optional<std::string> capture_sha256;
};
struct FrameInput {
    std::uint64_t map_epoch = 0, device_epoch = 0, frame_id = 0;
    std::uint32_t render_pass = 0, stage = 0;
    FrameProvenance provenance;
    Conventions conventions;
    Camera camera;
    // Producer must establish this from a complete, same-thread Present window;
    // this flag is an assertion, not independent proof of synchronization.
    bool synchronized = false;
    std::vector<MeshSelection> meshes;
    std::vector<BspSelection> bsp;
};
struct ResidentMesh {
    MeshSelection selection;
    std::shared_ptr<const Resource> resource;
};
struct ResidentBsp {
    BspSelection selection;
    std::shared_ptr<const Resource> resource;
};
struct Snapshot {
    std::string map, map_sha256;
    std::uint64_t map_epoch = 0, device_epoch = 0, frame_id = 0;
    // Advances when a published frame is invalidated within the same epoch.
    std::uint64_t continuity = 0;
    std::uint32_t render_pass = 0, stage = 0;
    FrameProvenance provenance;
    Conventions conventions;
    Camera camera;
    std::unordered_map<std::string, Material> materials;
    std::vector<ResidentMesh> meshes;
    std::vector<ResidentBsp> bsp;
};
struct Epochs {
    std::uint64_t map = 0, device = 0;
    bool ready = false;
};
// A newer ordinary frame can follow an already submitted one. An intervening
// invalidation, map/device change or different session breaks continuity.
bool DisplayContinuityMatches(const Snapshot& earlier, const Snapshot& latest) noexcept;

// All operations are synchronized. A reader can retain a snapshot while the
// producer publishes another one; immutable resource data is shared. Consumers
// must synchronize a device-epoch check with their own actual GPU submission.
class SceneResidency {
public:
    // Call on every observed map unload/load, including reloading the same map.
    bool BeginMap(std::uint64_t expected_map_epoch, const std::string& map, const std::string& map_sha256);
    // Call before forwarding every Reset, whether it later succeeds or fails.
    std::uint64_t BeginReset(); // zero means no reset ticket
    bool EndReset(std::uint64_t ticket, bool succeeded);
    bool AddMaterial(std::uint64_t map_epoch, Material material);
    bool AddResource(std::uint64_t map_epoch, Resource resource);
    bool Publish(const FrameInput& frame);
    // Invalidate the last displayable frame after a rejected Present interval.
    // Epoch and frame arguments prevent a late event from erasing a newer frame.
    bool DiscardFrame(std::uint64_t map_epoch, std::uint64_t device_epoch, std::uint64_t through_frame_id);
    Epochs epochs() const;
    // Returns the immutable resident object only for the requested map epoch.
    std::shared_ptr<const Resource> FindSceneResource(std::uint64_t map_epoch, const std::string& id) const;
    std::shared_ptr<const Snapshot> current() const;
    // Calls submit only while the exact snapshot remains current and its epoch
    // is ready. The callback must not call back into SceneResidency, block for
    // GPU completion, or throw. It may enqueue GPU work and return promptly.
    bool SubmitIfCurrent(const std::shared_ptr<const Snapshot>& expected,
                         bool (*submit)(void*) noexcept, void* context) noexcept;

private:
    struct Identity { std::uint64_t generation; std::string resource_id; };
    mutable std::mutex mutex_;
    std::string map_, map_sha256_;
    std::uint64_t map_epoch_ = 0, device_epoch_ = 0, last_frame_ = 0;
    std::uint64_t continuity_ = 1;
    bool ready_ = true, resetting_ = false, has_frame_ = false;
    std::size_t geometry_bytes_ = 0;
    std::optional<FrameProvenance> provenance_;
    std::optional<Conventions> conventions_;
    std::unordered_map<std::string, Material> materials_;
    std::unordered_map<std::string, std::shared_ptr<const Resource>> resources_;
    std::unordered_map<std::string, Identity> identities_;
    std::shared_ptr<const Snapshot> snapshot_;
};

} // namespace t3render::live
