#include "capture.hpp"
#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" IDirect3D8* Proxy_Direct3DCreate8(UINT);
extern "C" const IID IID_IDirect3D8{1}, IID_IDirect3DDevice8{2};
static std::vector<std::string> events;
static bool enabled = false, writable = true, reject_swap = false, recursive = false;
static bool create_fails = false, throw_capture = false;
static std::atomic<bool> stress{false};
static std::atomic<int> captured_draws{0}, forwarded_draws{0};
static int failures = 0;
static IDirect3DDevice8 devices[2];
static IDirect3DDevice8* last_freed;
static int next_device = 0;
static D3DPRESENT_PARAMETERS* observed_params;
static const void* observed_indices;
static const void* observed_vertices;
static t3render::DrawCall observed_call;
static IDirect3D8 factory;
static int real_calls = 0;
static thread_local DWORD last_error = 0;
static DWORD draw_up_input_error = 0;
static DWORD create_input_error = 0, reset_input_error = 0, present_input_error = 0;

extern "C" size_t VirtualQuery(void* p,MEMORY_BASIC_INFORMATION* out,size_t) { if(!writable) return 0; out->BaseAddress=p; out->RegionSize=sizeof(void*); out->State=MEM_COMMIT; out->Protect=PAGE_READWRITE; return sizeof(*out); }
extern "C" void* InterlockedCompareExchangePointer(void* volatile* p,void* x,void* compare) { void* prior=*p; if(reject_swap && x) {reject_swap=false; return reinterpret_cast<void*>(1);} if(prior==compare) *p=x; return prior; }
extern "C" void OutputDebugStringA(const char*) { ++failures; }
extern "C" DWORD GetLastError() { return last_error; }
extern "C" void SetLastError(DWORD value) { last_error = value; }
extern "C" UINT GetSystemDirectoryW(wchar_t* out,UINT) { const wchar_t path[]=L"sys"; std::memcpy(out,path,sizeof path); return 3; }
extern "C" HMODULE LoadLibraryW(const wchar_t*) { return reinterpret_cast<void*>(1); }
extern "C" int FreeLibrary(HMODULE) { return 1; }
extern "C" int GetModuleHandleExW(DWORD,LPCWSTR,HMODULE* out) { *out=reinterpret_cast<void*>(2); return 1; }
static IDirect3D8* real_create(UINT version) { ++real_calls; assert(version==220); create_input_error=GetLastError(); if(recursive) {recursive=false; assert(Proxy_Direct3DCreate8(version)==nullptr);} SetLastError(4242); return &factory; }
extern "C" void* GetProcAddress(HMODULE,const char*) { return reinterpret_cast<void*>(&real_create); }
namespace t3render {
bool CaptureConfigured() { SetLastError(991); return enabled; }
void CaptureDevice(IDirect3DDevice8*,const D3DPRESENT_PARAMETERS* p) { observed_params=const_cast<D3DPRESENT_PARAMETERS*>(p); events.push_back("capture-device"); }
void CaptureDeviceReleased(IDirect3DDevice8* d) { assert(d==last_freed); events.push_back("capture-release"); }
void CaptureDraw(IDirect3DDevice8*,const DrawCall& d) {
 SetLastError(991);
 if (stress) { ++captured_draws; return; }
 if (throw_capture) throw std::runtime_error("capture failed");
 observed_call=d; events.push_back("capture-draw");
}
void CaptureReset(IDirect3DDevice8*,const D3DPRESENT_PARAMETERS* p,HRESULT hr) { SetLastError(991); assert(hr==12); observed_params=const_cast<D3DPRESENT_PARAMETERS*>(p); events.push_back("capture-reset"); }
void CapturePresent(IDirect3DDevice8*,HRESULT hr) { SetLastError(991); assert(hr==13); events.push_back("capture-present"); }
}
static HRESULT query_factory(IDirect3D8* f,REFIID,void** out) { *out=f; ++f->refs; return 0; }
static ULONG add_factory(IDirect3D8* f) { return ++f->refs; }
static ULONG release_factory(IDirect3D8* f) { return --f->refs; }
static HRESULT make_device(IDirect3D8*,UINT,D3DDEVTYPE,HWND,DWORD,D3DPRESENT_PARAMETERS*,IDirect3DDevice8** out) {
 events.push_back("create-device");
 if (create_fails) return -123;
 *out=&devices[next_device]; return 0;
}
static HRESULT query_device(IDirect3DDevice8* d,REFIID,void** out) { *out=d; ++d->refs; return 0; }
static ULONG add_device(IDirect3DDevice8* d) { return ++d->refs; }
static ULONG release_device(IDirect3DDevice8* d) { ULONG refs=--d->refs; if (!refs) last_freed=d; return refs; }
static HRESULT reset(IDirect3DDevice8*,D3DPRESENT_PARAMETERS*) { reset_input_error=GetLastError(); SetLastError(912); events.push_back("reset"); return 12; }
static HRESULT present(IDirect3DDevice8*,const RECT*,const RECT*,HWND,const RGNDATA*) { present_input_error=GetLastError(); SetLastError(913); events.push_back("present"); return 13; }
static HRESULT draw(IDirect3DDevice8*,int,UINT,UINT) {
 if (stress) { ++forwarded_draws; return 14; }
 events.push_back("draw"); return 14;
}
static HRESULT draw_indexed(IDirect3DDevice8*,int,UINT,UINT,UINT,UINT) { events.push_back("draw-indexed"); return 15; }
static HRESULT draw_up(IDirect3DDevice8*,int,UINT,const void* v,UINT) { draw_up_input_error=GetLastError(); SetLastError(909); observed_vertices=v; events.push_back("draw-up"); return 16; }
static HRESULT draw_indexed_up(IDirect3DDevice8*,int,UINT,UINT,UINT,const void* i,int,const void* v,UINT) { observed_indices=i; observed_vertices=v; events.push_back("draw-indexed-up"); return 17; }
static IDirect3D8Vtbl factory_vt{query_factory,add_factory,release_factory,make_device};
static IDirect3DDevice8Vtbl device_vt{query_device,add_device,release_device,reset,present,draw,draw_indexed,draw_up,draw_indexed_up};
int main() {
 factory={&factory_vt,1}; devices[0]={&device_vt,1}; devices[1]={&device_vt,1};
 SetLastError(101);
 assert(Proxy_Direct3DCreate8(220)==&factory && factory.lpVtbl==&factory_vt);
 assert(create_input_error==101 && GetLastError()==4242);
 enabled=true; writable=false; assert(Proxy_Direct3DCreate8(220)==&factory && factory.lpVtbl==&factory_vt);
 writable=true; reject_swap=true; assert(Proxy_Direct3DCreate8(220)==&factory && factory.lpVtbl==&factory_vt);
 recursive=true; assert(Proxy_Direct3DCreate8(220)==&factory && factory.lpVtbl!=&factory_vt); assert(real_calls==4);
 IDirect3D8* f=&factory; void* queried=nullptr;
 assert(f->lpVtbl->QueryInterface(f,IID_IDirect3D8,&queried)==0 && queried==f); assert(f->lpVtbl->Release(f)==1);
 D3DPRESENT_PARAMETERS params{23}; IDirect3DDevice8* d=nullptr;
 create_fails=true;
 assert(f->lpVtbl->CreateDevice(f,0,0,nullptr,0,&params,&d)==-123 && d==nullptr);
 assert(events==std::vector<std::string>({"create-device"})); events.clear(); create_fails=false;
 writable=false;
 assert(f->lpVtbl->CreateDevice(f,0,0,nullptr,0,&params,&d)==0 && d==&devices[0] && d->lpVtbl==&device_vt);
 assert(events==std::vector<std::string>({"create-device"})); events.clear(); writable=true;
 assert(f->lpVtbl->CreateDevice(f,0,0,nullptr,0,&params,&d)==0 && d==&devices[0]);
 assert(events==std::vector<std::string>({"create-device","capture-device"})); assert(observed_params==&params); events.clear();
 assert(d->lpVtbl->QueryInterface(d,IID_IDirect3DDevice8,&queried)==0 && queried==d); assert(d->lpVtbl->Release(d)==1);
 assert(d->lpVtbl->DrawPrimitive(d,4,8,9)==14); assert(observed_call.kind==t3render::DrawKind::Primitive && observed_call.start_vertex==8 && observed_call.primitive_count==9);
 assert(events==std::vector<std::string>({"capture-draw","draw"})); events.clear();
 throw_capture=true; assert(d->lpVtbl->DrawPrimitive(d,4,8,9)==14); throw_capture=false;
 assert(events==std::vector<std::string>({"draw"})); events.clear();
 stress=true;
 std::vector<std::thread> workers;
 for (int n=0; n<4; ++n) workers.emplace_back([d] { for (int i=0; i<200; ++i) assert(d->lpVtbl->DrawPrimitive(d,4,8,9)==14); });
 for (auto& worker : workers) worker.join();
 stress=false; assert(captured_draws==800 && forwarded_draws==800);
 assert(d->lpVtbl->DrawIndexedPrimitive(d,4,5,6,7,8)==15); assert(observed_call.kind==t3render::DrawKind::IndexedPrimitive && observed_call.min_vertex_index==5 && observed_call.num_vertices==6 && observed_call.start_index==7 && observed_call.primitive_count==8);
 assert(events==std::vector<std::string>({"capture-draw","draw-indexed"})); events.clear();
 int vertices=1, indices=2;
 SetLastError(123);
 assert(d->lpVtbl->DrawPrimitiveUP(d,4,5,&vertices,24)==16); assert(observed_vertices==&vertices && observed_call.vertex_data==&vertices && !observed_call.index_data && observed_call.kind==t3render::DrawKind::PrimitiveUP && observed_call.vertex_stride==24);
 assert(draw_up_input_error==123 && GetLastError()==909);
 assert(events==std::vector<std::string>({"capture-draw","draw-up"})); events.clear();
 assert(d->lpVtbl->DrawIndexedPrimitiveUP(d,4,5,6,7,&indices,9,&vertices,24)==17); assert(observed_indices==&indices && observed_vertices==&vertices && observed_call.vertex_data==&vertices && observed_call.index_data==&indices && observed_call.kind==t3render::DrawKind::IndexedPrimitiveUP && observed_call.index_format==9);
 assert(events==std::vector<std::string>({"capture-draw","draw-indexed-up"})); events.clear();
 SetLastError(102);
 assert(d->lpVtbl->Reset(d,&params)==12); assert(reset_input_error==102 && GetLastError()==912); assert(observed_params==&params); assert(events==std::vector<std::string>({"reset","capture-reset"})); events.clear();
 SetLastError(103);
 assert(d->lpVtbl->Present(d,nullptr,nullptr,nullptr,nullptr)==13); assert(present_input_error==103 && GetLastError()==913); assert(events==std::vector<std::string>({"present","capture-present"})); events.clear();
 next_device=1; IDirect3DDevice8* d2=nullptr; assert(f->lpVtbl->CreateDevice(f,0,0,nullptr,0,&params,&d2)==0 && d2==&devices[1]); assert(d2->lpVtbl!=d->lpVtbl);
 events.clear(); assert(d->lpVtbl->Release(d)==0);
 assert(events==std::vector<std::string>({"capture-release"}));
 devices[0]={&device_vt,1}; next_device=0; assert(f->lpVtbl->CreateDevice(f,0,0,nullptr,0,&params,&d)==0 && d==&devices[0] && d->lpVtbl!=&device_vt);
 assert(d->lpVtbl->Release(d)==0); assert(d2->lpVtbl->Release(d2)==0); assert(f->lpVtbl->Release(f)==0);
 assert(failures>=3); std::cout << "proxy forwarding and lifecycle mock: passed\n";
}
