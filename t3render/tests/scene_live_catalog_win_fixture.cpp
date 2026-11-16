#include "../src/scene_live_catalog_win.hpp"
#include <windows.h>
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc!=3) return 2;
    t3render::live::LoadedCatalog catalog;
    SetLastError(12345);
    if (!t3render::live::LoadCatalogFile(argv[1],catalog) || GetLastError()!=12345 || !catalog.residency ||
        !catalog.binding.bed || catalog.binding.bsp.empty()) return 3;
    auto original=catalog.residency;
    SetLastError(54321);
    if (t3render::live::LoadCatalogFile(argv[2],catalog) || GetLastError()!=54321 || catalog.residency!=original) return 4;
    std::printf("catalog_win_fixture=ok bsp=%zu vertices=%zu\n",catalog.binding.bsp.size(),
                catalog.binding.bed->geometry.positions.size());
    return 0;
}
