#include "../src/scene_benchmark.hpp"
#include "../src/scene_live_flat.hpp"
#include "../src/scene_live_lighting.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace t3render::live;
namespace {
bool SuppliedDigest(const std::uint8_t*,std::size_t,std::uint8_t* output,void* context) noexcept {
    std::memcpy(output,context,32);return true;
}
int Digit(char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    return -1;
}
bool Renderable(const Snapshot& snapshot,const RawLightFrame& raw,const LoadedCatalog& catalog,bool on) {
    ResidentFlatFrame frame;
    if (!BuildResidentFlatFrame(snapshot,frame,catalog.bed_override.get(),true,false,nullptr,
                                bool(catalog.bedroom_materials->original_materials))) {
        std::cerr<<"cannot build resident frame\n";return false;
    }
    if (frame.override_instances!=1 || !frame.bedroom_material_runs || frame.draws.empty()) {
        std::cerr<<"missing bed or bedroom material selections\n";return false;
    }
    const auto base=frame.draws.size();
    AppendCandlePreview(snapshot,raw,on,frame,14);AppendCandlePreview(snapshot,raw,true,frame,6);
    unsigned selected=0,other=0,selected_flames=0,other_flames=0;
    for (std::size_t i=base;i<frame.draws.size();++i) {
        const auto& draw=frame.draws[i];
        if (draw.preview_candle_actor==14) {++selected;if (draw.emission>0) ++selected_flames;}
        if (draw.preview_candle_actor==6) {++other;if (draw.emission>0) ++other_flames;}
    }
    if (selected!=(on ? 3u : 2u) || other!=3 || selected_flames!=(on ? 1u : 0u) || other_flames!=1) {
        std::cerr<<"missing candle holder or flame geometry\n";return false;
    }
    return true;
}
}
int main(int argc,char** argv) {
    if (argc!=3 || std::strlen(argv[2])!=64) return 2;
    std::uint8_t digest[32]{};
    for (unsigned i=0;i<32;++i) {
        const auto a=Digit(argv[2][i*2]),b=Digit(argv[2][i*2+1]);if (a<0 || b<0) return 2;
        digest[i]=static_cast<std::uint8_t>((a<<4)|b);
    }
    std::vector<std::uint8_t> catalog((std::istreambuf_iterator<char>(std::cin)),std::istreambuf_iterator<char>());
    if (catalog.size()>kMaxCatalogBytes) return 2;
    LoadedCatalog loaded;
    if (!LoadCatalog(catalog.data(),catalog.size(),SuppliedDigest,digest,loaded)) {std::cerr<<"invalid catalog\n";return 1;}
    if (std::strcmp(argv[1],"--list-actors")==0) {
        std::vector<std::pair<std::string,const StaticMeshCandidate*>> actors;
        for (const auto& [id,candidate]:loaded.binding.static_mesh_actors) {
            if (!candidate.resource) return 1;
            actors.emplace_back(id,&candidate);
        }
        std::sort(actors.begin(),actors.end());
        for (const auto& [id,candidate]:actors)
            std::cout<<id<<' '<<candidate->resource->geometry.sections.size()<<' '<<candidate->resource->id<<'\n';
        return 0;
    }
    std::ifstream input(argv[1],std::ios::binary);BenchmarkScene scene;
    if (!input || !LoadBenchmarkScene(input,loaded,scene)) {std::cerr<<"invalid scene packet\n";return 1;}
    if (!Renderable(*scene.on,scene.on_lights,loaded,true) || !Renderable(*scene.off,scene.off_lights,loaded,false)) return 1;
    std::cout<<"t3bench_scene_valid\n";return 0;
}
