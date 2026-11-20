#include "scene_benchmark.hpp"
#include "scene_live_catalog_win.hpp"
#include "scene_benchmark_win.hpp"
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

using namespace t3render::live;
namespace {
bool WriteAll(HANDLE file,const void* data,std::size_t size) {
    auto bytes=static_cast<const std::uint8_t*>(data);
    while (size) {
        DWORD written=0;const DWORD chunk=static_cast<DWORD>(std::min<std::size_t>(size,1u<<20));
        if (!WriteFile(file,bytes,chunk,&written,nullptr) || !written) return false;
        bytes+=written;size-=written;
    }
    return true;
}
bool WriteImage(const wchar_t* path,const Camera& camera,const std::vector<std::uint8_t>& pixels) {
    if (pixels.size()!=std::size_t(camera.width)*camera.height*4) return false;
    const auto file=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) return false;
    const auto header=std::string("P6\n")+std::to_string(camera.width)+" "+std::to_string(camera.height)+"\n255\n";
    std::vector<std::uint8_t> rgb;rgb.reserve(pixels.size()/4*3);
    for (std::size_t i=0;i<pixels.size();i+=4) rgb.insert(rgb.end(),{pixels[i],pixels[i+1],pixels[i+2]});
    const bool ok=WriteAll(file,header.data(),header.size()) && WriteAll(file,rgb.data(),rgb.size());
    return CloseHandle(file) && ok;
}
}

int wmain(int argc,wchar_t** argv) {
    if (argc!=6) {std::cerr<<"usage: t3bench_renderer catalog scene flat on off\n";return 2;}
    LoadedCatalog catalog;
    if (!LoadCatalogFile(argv[1],catalog)) {std::cerr<<"invalid catalog\n";return 1;}
    auto input=_wfopen(argv[2],L"rb");
    if (!input) {std::cerr<<"cannot open benchmark scene\n";return 1;}
    std::string bytes;char block[4096];std::size_t n=0;
    while ((n=std::fread(block,1,sizeof(block),input))!=0 && bytes.size()<=4u*1024u*1024u) bytes.append(block,n);
    const bool read_ok=!std::ferror(input) && std::feof(input) && bytes.size()<=4u*1024u*1024u;
    std::fclose(input);
    BenchmarkScene scene;std::istringstream stream(bytes);
    if (!read_ok || !LoadBenchmarkScene(stream,catalog,scene)) {std::cerr<<"invalid benchmark scene\n";return 1;}
    BenchmarkPackage package;package.bed=catalog.bed_override;package.materials=catalog.bedroom_materials;
    BenchmarkImages images;
    if (!RenderBenchmark(package,scene,images)) {std::cerr<<"D3D12 render failed\n";return 1;}
    for (unsigned i=0;i<3;++i)
        if (!WriteImage(argv[3+i],scene.on->camera,images.pixels[i])) {std::cerr<<"image write failed\n";return 1;}
    std::cout<<std::fixed<<std::setprecision(3)<<"{\"cpu_readback_ms\":["<<images.cpu_readback_ms[0]<<','<<images.cpu_readback_ms[1]<<','<<images.cpu_readback_ms[2]
             <<"],\"draws\":["<<images.draws[0]<<','<<images.draws[1]<<','<<images.draws[2]<<"],\"triangles\":["
             <<images.triangles[0]<<','<<images.triangles[1]<<','<<images.triangles[2]<<"],\"shadowed_lights\":["
             <<images.shadowed_lights[0]<<','<<images.shadowed_lights[1]<<','<<images.shadowed_lights[2]<<"]}\n";
    return 0;
}
