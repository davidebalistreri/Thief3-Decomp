#pragma once

#include "scene_live_catalog.hpp"

namespace t3render::live {
// Reads one bounded local catalog and verifies its SHA-256 trailer using
// Windows CryptoAPI. All errors leave output unchanged and preserve LastError.
bool LoadCatalogFile(const wchar_t* path, LoadedCatalog& output) noexcept;
} // namespace t3render::live
