#include "../src/scene_live_catalog_win.hpp"
#include <windows.h>
#include <cstdio>
#include <cwchar>

int wmain(int argc, wchar_t** argv) {
    if (argc!=3 && (argc!=4 || std::wcscmp(argv[3],L"--bedless")!=0)) return 2;
    const bool bedless=argc==4;
    t3render::live::LoadedCatalog catalog;
    SetLastError(12345);
    if (!t3render::live::LoadCatalogFile(argv[1],catalog) || GetLastError()!=12345 || !catalog.residency ||
        (catalog.binding.bsp.empty() && catalog.binding.static_mesh_actors.empty() && !catalog.binding.bed) ||
        (bedless ? bool(catalog.binding.bed) : !catalog.binding.bed)) return 3;
    auto original=catalog.residency;
    SetLastError(54321);
    if (t3render::live::LoadCatalogFile(argv[2],catalog) || GetLastError()!=54321 || catalog.residency!=original) return 4;
    std::printf("catalog_win_fixture=ok bsp=%zu vertices=%zu\n",catalog.binding.bsp.size(),
                catalog.binding.bed ? catalog.binding.bed->geometry.positions.size() : 0);
    return 0;
}
