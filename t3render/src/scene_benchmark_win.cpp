#include "scene_benchmark_win.hpp"
#include "scene_live_dx12.hpp"
#include "scene_live_lighting.hpp"
#include <windows.h>
#include <wincrypt.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace t3render::live {
namespace {
bool Digest(const std::uint8_t* bytes,std::size_t size,std::uint8_t* output,void*) noexcept {
    if (!bytes || !size || size>kMaxCatalogBytes || !output) return false;
    HCRYPTPROV provider=0;HCRYPTHASH hash=0;bool ok=false;
    if (CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT|CRYPT_SILENT) &&
        CryptCreateHash(provider,CALG_SHA_256,0,0,&hash) && CryptHashData(hash,bytes,static_cast<DWORD>(size),0)) {
        DWORD length=32;ok=CryptGetHashParam(hash,HP_HASHVAL,output,&length,0) && length==32;
    }
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider,0);
    return ok;
}
std::string Hex(const std::vector<std::uint8_t>& data) {
    std::uint8_t hash[32]{};if (!Digest(data.data(),data.size(),hash,nullptr)) return {};
    constexpr char digits[]="0123456789abcdef";std::string result;
    for (const auto value:hash) {result.push_back(digits[value>>4]);result.push_back(digits[value&15]);}
    return result;
}
bool Read(const std::wstring& path,std::size_t limit,std::vector<std::uint8_t>& output) {
    const auto file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER length{};
    bool ok=GetFileSizeEx(file,&length) && length.QuadPart>0 && static_cast<std::uint64_t>(length.QuadPart)<=limit;
    std::vector<std::uint8_t> bytes;
    if (ok) {
        bytes.resize(static_cast<std::size_t>(length.QuadPart));std::size_t offset=0;
        while (offset<bytes.size()) {
            DWORD n=0;const auto count=static_cast<DWORD>(std::min<std::size_t>(bytes.size()-offset,1u<<20));
            if (!ReadFile(file,bytes.data()+offset,count,&n,nullptr) || !n) {ok=false;break;}
            offset+=n;
        }
        std::uint8_t extra=0;DWORD n=0;
        if (ok && (!ReadFile(file,&extra,1,&n,nullptr) || n)) ok=false;
    }
    if (!CloseHandle(file)) ok=false;
    if (ok) output=std::move(bytes);
    return ok;
}
bool Ppm(const std::vector<std::uint8_t>& bytes,std::uint32_t width,std::uint32_t height,std::vector<std::uint8_t>& rgb) {
    const auto header=std::string("P6\n")+std::to_string(width)+" "+std::to_string(height)+"\n255\n";
    if (bytes.size()!=header.size()+std::size_t(width)*height*3 ||
        !std::equal(header.begin(),header.end(),bytes.begin())) return false;
    rgb.assign(bytes.begin()+header.size(),bytes.end());return true;
}
bool Write(const std::wstring& path,const std::vector<std::uint8_t>& bytes) {
    const auto file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) return false;
    std::size_t offset=0;bool ok=true;
    while (offset<bytes.size()) {
        DWORD n=0;const auto count=static_cast<DWORD>(std::min<std::size_t>(bytes.size()-offset,1u<<20));
        if (!WriteFile(file,bytes.data()+offset,count,&n,nullptr) || !n) {ok=false;break;}
        offset+=n;
    }
    return CloseHandle(file) && ok;
}
std::vector<std::uint8_t> Rgb(const std::vector<std::uint8_t>& rgba) {
    std::vector<std::uint8_t> out;out.reserve(rgba.size()/4*3);
    for (std::size_t i=0;i<rgba.size();i+=4) out.insert(out.end(),{rgba[i],rgba[i+1],rgba[i+2]});
    return out;
}
bool WritePpm(const std::wstring& path,std::uint32_t width,std::uint32_t height,const std::vector<std::uint8_t>& rgb) {
    if (rgb.size()!=std::size_t(width)*height*3) return false;
    const auto header=std::string("P6\n")+std::to_string(width)+" "+std::to_string(height)+"\n255\n";
    std::vector<std::uint8_t> bytes(header.begin(),header.end());bytes.insert(bytes.end(),rgb.begin(),rgb.end());
    return Write(path,bytes);
}
bool WriteBmp(const std::wstring& path,std::uint32_t width,std::uint32_t height,const std::vector<std::uint8_t>& rgb) {
    if (rgb.size()!=std::size_t(width)*height*3) return false;
    const auto stride=(width*3u+3u)&~3u;const auto pixel_bytes=stride*height;
    std::vector<std::uint8_t> bytes;bytes.reserve(std::size_t(pixel_bytes)+54);
    const auto u16=[&](std::uint16_t value){bytes.push_back(value&255);bytes.push_back(value>>8);};
    const auto u32=[&](std::uint32_t value){u16(value&0xffff);u16(value>>16);};
    bytes.push_back('B');bytes.push_back('M');u32(54u+pixel_bytes);u16(0);u16(0);u32(54);
    u32(40);u32(width);u32(height);u16(1);u16(24);u32(0);u32(pixel_bytes);
    u32(0);u32(0);u32(0);u32(0);
    for (std::uint32_t y=height;y>0;--y) {
        const auto start=std::size_t(y-1)*width*3;
        for (std::uint32_t x=0;x<width;++x) {
            const auto index=start+std::size_t(x)*3;
            bytes.insert(bytes.end(),{rgb[index+2],rgb[index+1],rgb[index]});
        }
        bytes.insert(bytes.end(),stride-width*3,0);
    }
    return Write(path,bytes);
}
void Difference(std::ostream& out,const std::vector<std::uint8_t>& a,const std::vector<std::uint8_t>& b) {
    std::uint64_t changed=0,total=0;
    for (std::size_t i=0;i<a.size();i+=3) {
        if (a[i]!=b[i] || a[i+1]!=b[i+1] || a[i+2]!=b[i+2]) ++changed;
        for (unsigned j=0;j<3;++j) total+=static_cast<unsigned>(std::abs(int(a[i+j])-int(b[i+j])));
    }
    out<<"{\"different_pixels\":"<<changed<<",\"mean_absolute_rgb\":"<<(double(total)/a.size())<<'}';
}
} // namespace

bool LoadBenchmarkPackage(const std::wstring& directory,BenchmarkPackage& output) noexcept {
    try {
        constexpr const char* names[]={"catalog.bin","scene.txt","d3d8.ppm","live-flat.ppm","live-on.ppm","live-off.ppm"};
        std::vector<std::uint8_t> integrity;
        if (!Read(directory+L"\\integrity.txt",4096,integrity)) return false;
        std::istringstream index(std::string(integrity.begin(),integrity.end()));
        std::vector<std::uint8_t> catalog,scene,original;
        BenchmarkPackage result;
        for (unsigned i=0;i<6;++i) {
            std::string name,expected;
            if (!(index>>name>>expected) || name!=names[i] || expected.size()!=64) return false;
            const std::wstring wide(name.begin(),name.end());std::vector<std::uint8_t> bytes;
            const std::size_t limit=i==0 ? kMaxCatalogBytes : i==1 ? 4u*1024u*1024u : 16u*1024u*1024u;
            if (!Read(directory+L"\\"+wide,limit,bytes) || Hex(bytes)!=expected) return false;
            if (i==0) {catalog=std::move(bytes);result.catalog_sha256=expected;}
            else if (i==1) {scene=std::move(bytes);result.scene_sha256=expected;}
            else if (i==2) {original=std::move(bytes);result.original_sha256=expected;}
        }
        index>>std::ws;if (!index.eof()) return false;
        LoadedCatalog loaded;if (!LoadCatalog(catalog.data(),catalog.size(),Digest,nullptr,loaded)) return false;
        std::istringstream packet(std::string(scene.begin(),scene.end()));
        if (!LoadBenchmarkScene(packet,loaded,result.scene) ||
            !Ppm(original,result.scene.on->camera.width,result.scene.on->camera.height,result.original_rgb)) return false;
        result.bed=loaded.bed_override;result.materials=loaded.bedroom_materials;
        output=std::move(result);return true;
    } catch (...) {return false;}
}

bool RenderBenchmark(const BenchmarkPackage& package,const BenchmarkScene& scene,BenchmarkImages& output) noexcept {
    try {
        if (!scene.on || !scene.off || !package.bed || !package.materials) return false;
        auto gpu=FlatD3D12::Create(nullptr,128u*1024u*1024u,package.materials,true,false);
        if (!gpu) return false;
        BenchmarkImages result;
        for (unsigned variant=0;variant<3;++variant) {
            const auto& snapshot=variant==2 ? *scene.off : *scene.on;
            const auto& raw=variant==2 ? scene.off_lights : scene.on_lights;
            ResidentFlatFrame frame;
            if (!BuildResidentFlatFrame(snapshot,frame,variant ? package.bed.get() : nullptr,variant!=0,false,
                                        nullptr,variant && bool(package.materials->original_materials))) return false;
            if (variant) {AppendCandlePreview(snapshot,raw,variant==1,frame,14);AppendCandlePreview(snapshot,raw,true,frame,6);}
            result.draws[variant]=static_cast<std::uint32_t>(frame.draws.size());
            for (const auto& draw:frame.draws) result.triangles[variant]+=draw.index_count/3;
            const auto start=std::chrono::steady_clock::now();
            const bool prepared=variant ? gpu->PrepareCandlePreview(snapshot,variant==1 ? &raw : nullptr,frame,true,&raw,
                                                                     false,3,CandlePreviewProfile::BedroomV01) :
                                          gpu->PrepareResident(frame,true);
            if (!prepared || !gpu->Submit() || !gpu->Finish(&result.pixels[variant])) return false;
            result.shadowed_lights[variant]=gpu->cache_stats().shadowed_lights;
            result.cpu_readback_ms[variant]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            if (result.pixels[variant].size()!=std::size_t(snapshot.camera.width)*snapshot.camera.height*4) return false;
        }
        output=std::move(result);return true;
    } catch (...) {return false;}
}

bool WriteBenchmarkImages(const std::wstring& directory,const BenchmarkPackage& package,
                          const BenchmarkScene& scene,const Snapshot& live,
                          std::size_t meshes,std::size_t bsp,const BenchmarkImages& images) noexcept {
    try {
        const auto w=scene.on->camera.width,h=scene.on->camera.height;
        std::vector<std::uint8_t> rgb[4];rgb[0]=package.original_rgb;
        for (unsigned i=0;i<3;++i) rgb[i+1]=Rgb(images.pixels[i]);
        for (const auto& item:rgb) if (item.size()!=std::size_t(w)*h*3) return false;
        constexpr const wchar_t* names[]={L"flat.ppm",L"on.ppm",L"off.ppm"};
        for (unsigned i=0;i<3;++i) if (!WritePpm(directory+L"\\"+names[i],w,h,rgb[i+1])) return false;
        std::vector<std::uint8_t> sheet;sheet.reserve(std::size_t(w)*h*12);
        for (std::uint32_t y=0;y<h;++y) for (const auto& item:rgb) {
            const auto begin=item.begin()+std::size_t(y)*w*3;sheet.insert(sheet.end(),begin,begin+std::size_t(w)*3);
        }
        if (!WritePpm(directory+L"\\comparison.ppm",w*4,h,sheet)) return false;
        if (!WriteBmp(directory+L"\\comparison.bmp",w*4,h,sheet)) return false;
        std::ostringstream report;report<<std::fixed<<std::setprecision(5)
            <<"{\"format\":\"t3bench-live-v1\",\"live_frame\":"<<live.frame_id
            <<",\"map_epoch\":"<<live.map_epoch<<",\"device_epoch\":"<<live.device_epoch
            <<",\"continuity\":"<<live.continuity<<",\"matched_live_meshes\":"<<meshes
            <<",\"matched_live_bsp\":"<<bsp<<",\"camera\":\"golden\",\"light_states\":\"recorded_candidates\""
            <<",\"package\":{\"catalog_sha256\":\""<<package.catalog_sha256<<"\",\"scene_sha256\":\""
            <<package.scene_sha256<<"\",\"d3d8_sha256\":\""<<package.original_sha256<<"\"}"
            <<",\"camera_view_bits\":[";
        for (unsigned i=0;i<16;++i) {
            std::uint32_t bits=0;std::memcpy(&bits,&scene.on->camera.view[i],sizeof(bits));
            if (i) report<<',';
            report<<bits;
        }
        report<<"],\"camera_projection_bits\":[";
        for (unsigned i=0;i<16;++i) {
            std::uint32_t bits=0;std::memcpy(&bits,&scene.on->camera.projection[i],sizeof(bits));
            if (i) report<<',';
            report<<bits;
        }
        std::uint32_t min_depth=0,max_depth=0;
        std::memcpy(&min_depth,&scene.on->camera.min_depth,sizeof(min_depth));
        std::memcpy(&max_depth,&scene.on->camera.max_depth,sizeof(max_depth));
        report<<"],\"viewport\":{\"x\":"<<scene.on->camera.x<<",\"y\":"<<scene.on->camera.y
            <<",\"width\":"<<scene.on->camera.width<<",\"height\":"<<scene.on->camera.height
            <<",\"min_depth_bits\":"<<min_depth<<",\"max_depth_bits\":"<<max_depth<<'}'
            <<",\"missing_transforms\":\"golden\",\"cpu_readback_ms\":["<<images.cpu_readback_ms[0]<<','
            <<images.cpu_readback_ms[1]<<','<<images.cpu_readback_ms[2]<<"],\"draws\":["<<images.draws[0]<<','
            <<images.draws[1]<<','<<images.draws[2]<<"],\"triangles\":["<<images.triangles[0]<<','
            <<images.triangles[1]<<','<<images.triangles[2]<<"],\"shadowed_lights\":["
            <<images.shadowed_lights[0]<<','<<images.shadowed_lights[1]<<','<<images.shadowed_lights[2]
            <<"],\"flat_vs_on\":";
        Difference(report,rgb[1],rgb[2]);report<<",\"on_vs_off\":";Difference(report,rgb[2],rgb[3]);
        report<<",\"d3d8_vs_flat\":";Difference(report,rgb[0],rgb[1]);
        report<<",\"d3d8_vs_on\":";Difference(report,rgb[0],rgb[2]);
        report<<",\"d3d8_vs_off\":";Difference(report,rgb[0],rgb[3]);report<<"}\n";
        const auto text=report.str();return Write(directory+L"\\metrics.json",{text.begin(),text.end()});
    } catch (...) {return false;}
}
} // namespace t3render::live
