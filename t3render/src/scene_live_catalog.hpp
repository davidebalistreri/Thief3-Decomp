#pragma once

#include "scene_live_binding.hpp"

namespace t3render::live {
constexpr std::size_t kMaxCatalogBytes = 64u * 1024u * 1024u;
// Caller supplies SHA-256 so the API-neutral loader does not depend on an OS
// crypto library. The digest covers every byte before the 32-byte trailer.
using CatalogDigest = bool (*)(const std::uint8_t*, std::size_t, std::uint8_t*, void*) noexcept;
struct LoadedCatalog {
    std::shared_ptr<SceneResidency> residency;
    BindingCatalog binding;
    Conventions conventions;
    std::string offline_scene_sha256;
};
// No state is published on failure. Catalog geometry is offline asset data.
bool LoadCatalog(const std::uint8_t* bytes, std::size_t size, CatalogDigest digest,
                 void* digest_context, LoadedCatalog& output);
} // namespace t3render::live
