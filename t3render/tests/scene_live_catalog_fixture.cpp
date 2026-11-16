#include "../src/scene_live_catalog.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <vector>

using namespace t3render::live;
namespace {
struct Expected { std::uint8_t bytes[32]{}; };
bool Digest(const std::uint8_t*,std::size_t,std::uint8_t* output,void* context) noexcept {
    std::memcpy(output,static_cast<Expected*>(context)->bytes,32);return true;
}
int hex(char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    return -1;
}
} // namespace
int main(int argc,char** argv) {
    if (argc!=2 || std::strlen(argv[1])!=64) return 3;
    Expected expected;
    for (unsigned i=0;i<32;++i) {
        const int a=hex(argv[1][2*i]),b=hex(argv[1][2*i+1]);
        if (a<0 || b<0) return 3;
        expected.bytes[i]=static_cast<std::uint8_t>((a<<4)|b);
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(std::cin)),{});
    LoadedCatalog catalog;catalog.offline_scene_sha256="unchanged";
    if (!LoadCatalog(bytes.data(),bytes.size(),Digest,&expected,catalog)) {
        if (catalog.offline_scene_sha256!="unchanged" || catalog.residency) return 4;
        std::cout<<"rejected\n";return 0;
    }
    const auto epoch=catalog.residency->epochs();
    const auto& bed=catalog.binding.bed;
    if (!bed || catalog.residency->FindResource(epoch.map,bed->id)!=bed ||
        catalog.binding.bsp.empty() || catalog.residency->FindResource(epoch.map,catalog.binding.bsp[0]->id)!=catalog.binding.bsp[0]) return 5;
    std::cout<<"accepted "<<catalog.binding.map<<" "<<catalog.binding.materials.size()<<" "
             <<catalog.binding.bsp.size()<<" "<<bed->geometry.positions.size()<<" "<<bed->geometry.indices.size()<<" "
             <<bed->geometry.normals.size()<<" "<<bed->geometry.uv0.size()<<" "<<bed->geometry.tangents.size()<<" "
             <<catalog.binding.bed_materials.size()<<" "<<catalog.offline_scene_sha256<<"\n";
}
