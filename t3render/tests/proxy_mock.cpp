#include "capture.hpp"
#include "../ir/buffer_registry.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using AllocationHook = void (*)();
static thread_local AllocationHook allocation_hook = nullptr, deallocation_hook = nullptr;
void* operator new(std::size_t size) {
 void* p=std::malloc(size ? size : 1);
 if (!p) throw std::bad_alloc();
 auto hook=allocation_hook; allocation_hook=nullptr; if (hook) hook();
 return p;
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept {
 auto hook=deallocation_hook; deallocation_hook=nullptr; if (hook) hook();
 std::free(p);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p,std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p,std::size_t) noexcept { ::operator delete(p); }

extern "C" IDirect3D8* Proxy_Direct3DCreate8(UINT);
extern "C" const IID IID_IUnknown{0}, IID_IDirect3D8{1}, IID_IDirect3DDevice8{2};
extern "C" const IID IID_IDirect3DResource8{3}, IID_IDirect3DVertexBuffer8{4}, IID_IDirect3DIndexBuffer8{5};
#ifdef T3RENDER_PROXY_TEST
t3render::ir::RegistryResult T3ProxyTestReadVertex(IDirect3DDevice8*, IDirect3DVertexBuffer8*, std::size_t, void*, std::size_t);
t3render::ir::RegistryResult T3ProxyTestReadIndex(IDirect3DDevice8*, IDirect3DIndexBuffer8*, std::size_t, void*, std::size_t);
#endif
static std::vector<std::string> events;
static bool enabled = false, writable = true, reject_swap = false, recursive = false;
static bool buffers_requested = false;
static bool declarations_requested = false, history_requested = false, invalid_history_option = false;
static bool vertex_programs_requested = false;
static bool declaration_testing = false, wants_draw = true;
static bool declaration_reset_fails = false, declaration_set_fails = false, declaration_delete_fails = false;
static bool declaration_create_fails = false, declaration_output_unreadable = false;
static bool declaration_getter_fails = false, declaration_draw_fails = false;
static int shader_getter_reentry = 0, shader_writer_reentry = 0;
static bool capture_device_reentry = false;
static int writer_entry_reentry = 0;
static IDirect3DDevice8* shader_writer_target = nullptr;
static bool observed_abandoned = false;
static std::vector<bool> observed_shader_stabilities;
static unsigned declaration_getter_count = 0;
static DWORD shader_value = 0, shader_handle = 0xf0000001, shader_input_error = 0, draw_input_error = 0;
static const void* unreadable_address = nullptr;
static t3render::ShaderSnapshot observed_shader;
static bool observed_shader_present = false, observed_shader_result = false, observed_shader_stable = false;
static int observed_shader_buffer_stable = -2;
static bool invalid_buffer_option = false;
static bool create_fails = false, throw_capture = false;
static bool stop_on_present = false, stop_on_capture_error = false;
static bool observed_buffer_observer = false, observed_buffer_observer_valid = false;
static bool observed_buffer_result = false, observed_buffer_stable = false;
static std::atomic<bool> stress{false};
static std::atomic<int> captured_draws{0}, forwarded_draws{0};
static std::atomic<bool> block_draw{false}, draw_entered{false}, allow_draw{false};
static std::atomic<bool> block_set_stream{false}, set_stream_entered{false}, allow_set_stream{false};
static std::atomic<std::uint64_t> next_token{1};
static int failures = 0;
static IDirect3DDevice8 devices[24];
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
static DWORD clear_input_error = 0;
static const D3DRECT* clear_rectangles = nullptr;
static DWORD clear_count = 0, clear_flags = 0, clear_color = 0, clear_stencil = 0;
static float clear_depth = 0;
static std::mutex fake_capture_mutex;
static IDirect3DDevice8* allocation_device = nullptr;
static bool deallocation_outside_writer = false;
static void ReenterFromAllocation() {
 assert(allocation_device->lpVtbl->DrawPrimitive(allocation_device,4,0,1)==14);
}
static void VerifyDeallocationOutsideWriter() {
 std::lock_guard<std::mutex> lock(fake_capture_mutex);
 deallocation_outside_writer=true;
}
static bool reentrant_capture = false;
static bool drop_binding_during_get = false;
static bool return_device_alias = false;
static bool device_query_alias = false;
static unsigned query_alias_index = 1;
static bool up_clears_stream = false;
static IDirect3DVertexBuffer8* bound_vertex = nullptr;
static IDirect3DIndexBuffer8* bound_index = nullptr;
static UINT bound_stride = 0, bound_base = 0;
static HRESULT create_vertex_result = 0, create_index_result = 0;
static HRESULT set_stream_result = 0, get_stream_result = 0, set_indices_result = 0, get_indices_result = 0;
static DWORD vertex_create_error = 0, index_create_error = 0, stream_set_error = 0, stream_get_error = 0;
static DWORD indices_set_error = 0, indices_get_error = 0;
#ifdef T3RENDER_STATEBLOCK_TEST_FIXTURE
static std::atomic<bool> block_stateblock{false}, stateblock_entered{false}, allow_stateblock{false};
static bool fail_stateblock = false;
static DWORD stateblock_input_error = 0;
static HRESULT stateblock_call(HRESULT success) {
 stateblock_input_error=GetLastError();
 if(block_stateblock) { stateblock_entered=true; while(!allow_stateblock) std::this_thread::yield(); }
 SetLastError(947);
 return fail_stateblock ? -98 : success;
}
#endif

template<class Interface, class Desc, int Type>
struct NativeBuffer final : Interface {
 IDirect3DDevice8* owner;
 ULONG refs=1;
 bool destroyed=false, locked=false;
 UINT size, stride_or_fvf;
 DWORD usage;
 D3DFORMAT format;
 D3DPOOL pool;
 std::array<BYTE, 128> bytes{};
 HRESULT lock_result=S_OK, unlock_result=S_OK;
 unsigned locks=0, unlocks=0;
 DWORD lock_input_error=0, unlock_input_error=0;
 NativeBuffer(IDirect3DDevice8* d,UINT length,DWORD u,UINT f,D3DPOOL p)
     : owner(d),size(length),stride_or_fvf(f),usage(u),format(f),pool(p) {
  owner->lpVtbl->AddRef(owner);
 }
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
  if(!out) return E_POINTER;
  const bool own = Type==D3DRTYPE_VERTEXBUFFER ? IsEqualIID(iid,IID_IDirect3DVertexBuffer8)
                                               : IsEqualIID(iid,IID_IDirect3DIndexBuffer8);
  // IID 99 models a raw, otherwise uninstrumented COM alias.
  if(!own && !IsEqualIID(iid,IID_IUnknown) && !IsEqualIID(iid,IID_IDirect3DResource8) && iid.id!=99) {
   *out=nullptr; return E_NOINTERFACE;
  }
  *out=static_cast<Interface*>(this); AddRef(); return S_OK;
 }
 ULONG STDMETHODCALLTYPE AddRef() override { assert(!destroyed && refs); return ++refs; }
 ULONG STDMETHODCALLTYPE Release() override {
  assert(!destroyed && refs); const ULONG left=--refs;
  if(!left) { destroyed=true; owner->lpVtbl->Release(owner); }
  return left;
 }
 HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8** out) override {
  if(!out) return E_POINTER;
  *out=return_device_alias ? &devices[1] : owner;
  (*out)->lpVtbl->AddRef(*out); return S_OK;
 }
 HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,const void*,DWORD,DWORD) override { return S_OK; }
 HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,void*,DWORD*) override { return S_OK; }
 HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return S_OK; }
 DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
 DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
 void STDMETHODCALLTYPE PreLoad() override {}
 D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return Type; }
 HRESULT STDMETHODCALLTYPE Lock(UINT offset,UINT length,BYTE** out,DWORD) override {
  lock_input_error=GetLastError(); SetLastError(931); ++locks;
  if(FAILED(lock_result)) return lock_result;
  if(!out || offset>size || length>size-offset || size>bytes.size()) return D3DERR_INVALIDCALL;
  *out=bytes.data()+offset; locked=true; return S_OK;
 }
 HRESULT STDMETHODCALLTYPE Unlock() override {
  unlock_input_error=GetLastError(); SetLastError(932); ++unlocks;
  if(FAILED(unlock_result)) return unlock_result;
  if(!locked) return D3DERR_INVALIDCALL;
  locked=false; return S_OK;
 }
 HRESULT STDMETHODCALLTYPE GetDesc(Desc* out) override {
  if(!out) return E_POINTER; *out={}; out->Type=Type; out->Usage=usage; out->Pool=pool; out->Size=size;
  if constexpr (Type==D3DRTYPE_VERTEXBUFFER) out->FVF=stride_or_fvf;
  else out->Format=format;
  return S_OK;
 }
};
using NativeVertex = NativeBuffer<IDirect3DVertexBuffer8,D3DVERTEXBUFFER_DESC,D3DRTYPE_VERTEXBUFFER>;
using NativeIndex = NativeBuffer<IDirect3DIndexBuffer8,D3DINDEXBUFFER_DESC,D3DRTYPE_INDEXBUFFER>;
static std::unique_ptr<NativeVertex> native_vertex;
static std::unique_ptr<NativeIndex> native_index;

extern "C" size_t VirtualQuery(void* p,MEMORY_BASIC_INFORMATION* out,size_t) { if(!writable || p==unreadable_address) return 0; out->BaseAddress=p; out->RegionSize=4096; out->State=MEM_COMMIT; out->Protect=PAGE_READWRITE; return sizeof(*out); }
extern "C" void* InterlockedCompareExchangePointer(void* volatile* p,void* x,void* compare) { void* prior=*p; if(reject_swap && x) {reject_swap=false; return reinterpret_cast<void*>(1);} if(prior==compare) *p=x; return prior; }
extern "C" void OutputDebugStringA(const char*) { ++failures; }
extern "C" DWORD GetLastError() { return last_error; }
extern "C" void SetLastError(DWORD value) { last_error = value; }
extern "C" UINT GetSystemDirectoryW(wchar_t* out,UINT) { const wchar_t path[]=L"sys"; std::memcpy(out,path,sizeof path); return 3; }
extern "C" DWORD GetEnvironmentVariableW(const wchar_t* name, wchar_t* value, DWORD size) {
 const bool buffers = !std::wcscmp(name,L"T3RENDER_CAPTURE_BUFFERS");
 const bool declarations = !std::wcscmp(name,L"T3RENDER_CAPTURE_DECLARATIONS");
 const bool history = !std::wcscmp(name,L"T3RENDER_CAPTURE_DECLARATION_HISTORY");
 const bool programs = !std::wcscmp(name,L"T3RENDER_CAPTURE_VERTEX_PROGRAMS");
 if ((!buffers || !buffers_requested) && (!declarations || !declarations_requested) && (!history || !history_requested) &&
     (!programs || !vertex_programs_requested)) return 0;
 if (size < 2) return 2;
 value[0]=((buffers && invalid_buffer_option) || (history && invalid_history_option)) ? L'0' : L'1'; value[1]=0; return 1;
}
extern "C" HANDLE GetCurrentProcess() { return reinterpret_cast<HANDLE>(1); }
extern "C" BOOL ReadProcessMemory(HANDLE,const void* source,void* destination,SIZE_T length,SIZE_T* copied) {
 if (!source || !destination) { if(copied) *copied=0; return 0; }
 std::memcpy(destination,source,length); if(copied) *copied=length; return 1;
}
extern "C" HMODULE LoadLibraryW(const wchar_t*) { return reinterpret_cast<void*>(1); }
extern "C" int FreeLibrary(HMODULE) { return 1; }
extern "C" int GetModuleHandleExW(DWORD,LPCWSTR,HMODULE* out) { *out=reinterpret_cast<void*>(2); return 1; }
static IDirect3D8* real_create(UINT version) { ++real_calls; assert(version==220); create_input_error=GetLastError(); if(recursive) {recursive=false; assert(Proxy_Direct3DCreate8(version)==nullptr);} SetLastError(4242); return &factory; }
extern "C" void* GetProcAddress(HMODULE,const char*) { return reinterpret_cast<void*>(&real_create); }
static void MockWriterEntry(int entry, IDirect3DDevice8* device) {
 if (writer_entry_reentry!=entry) return;
 std::lock_guard<std::mutex> lock(fake_capture_mutex);
 writer_entry_reentry=0;
 allocation_device=device;
 allocation_hook=ReenterFromAllocation;
 void* scratch=::operator new(32);
 assert(!allocation_hook);
 ::operator delete(scratch);
}

namespace t3render {
bool CaptureConfigured() { SetLastError(991); MockWriterEntry(1,&devices[0]); return enabled; }
bool CaptureWantsDraw(IDirect3DDevice8* d) { SetLastError(991); MockWriterEntry(8,d); return wants_draw; }
void CaptureDevice(IDirect3DDevice8* d,const D3DPRESENT_PARAMETERS* p) {
 observed_params=const_cast<D3DPRESENT_PARAMETERS*>(p); events.push_back("capture-device");
 if (capture_device_reentry) {
  std::lock_guard<std::mutex> lock(fake_capture_mutex);
  capture_device_reentry=false;
  assert(d->lpVtbl->DrawPrimitive(d,4,0,1)==14);
 }
}
void CaptureAbandonDevice(IDirect3DDevice8*) {
 std::lock_guard<std::mutex> lock(fake_capture_mutex);
 observed_abandoned=true; enabled=false; SetLastError(991);
}
void CaptureDeviceReleased(IDirect3DDevice8* d) {
 std::unique_lock<std::mutex> lock(fake_capture_mutex, std::defer_lock);
 if (reentrant_capture) lock.lock();
 assert(d==last_freed); events.push_back("capture-release");
}
std::uint64_t CaptureDraw(IDirect3DDevice8* device,const DrawCall& d) {
 SetLastError(991);
 if (stress) { ++captured_draws; return next_token++; }
 if (throw_capture) {
  if(stop_on_capture_error) enabled=false;
  throw std::runtime_error("capture failed");
 }
 if (reentrant_capture) {
  std::lock_guard<std::mutex> lock(fake_capture_mutex);
  IDirect3DVertexBuffer8* vertex=nullptr; IDirect3DIndexBuffer8* index=nullptr;
  UINT stride=0, base=0;
  assert(device->lpVtbl->GetStreamSource(device,0,&vertex,&stride)==S_OK);
  assert(device->lpVtbl->GetIndices(device,&index,&base)==S_OK);
  if(vertex) vertex->Release();
  if(index) index->Release();
 }
 std::unique_lock<std::mutex> writer_lock(fake_capture_mutex,std::defer_lock);
 if (shader_writer_reentry) {
  writer_lock.lock();
  const int op=shader_writer_reentry; shader_writer_reentry=0;
  auto* target=shader_writer_target ? shader_writer_target : device;
  if (op==1) assert(target->lpVtbl->SetVertexShader(target,0xf0000002)==S_OK);
  if (op==2) assert(target->lpVtbl->DrawPrimitive(target,4,0,1)==14);
  if (op==3) { const DWORD decl[]={1u<<29,0xffffffffu}; DWORD handle=0;
               assert(target->lpVtbl->CreateVertexShader(target,decl,nullptr,&handle,0)==S_OK); }
  if (op==4) assert(target->lpVtbl->DeleteVertexShader(target,shader_handle)==S_OK);
  if (op==5) { D3DPRESENT_PARAMETERS params{23}; assert(target->lpVtbl->Reset(target,&params)==12); }
  if (op==6) assert(target->lpVtbl->ApplyStateBlock(target,7)==23);
  if (op==7) { device_query_alias=true; void* alias=nullptr;
               assert(target->lpVtbl->QueryInterface(target,IID_IDirect3DDevice8,&alias)==S_OK); }
  if (op==8) {
   deallocation_hook=VerifyDeallocationOutsideWriter;
   assert(target->lpVtbl->Release(target)==0);
  }
  if (op==9) assert(target->lpVtbl->Present(target,nullptr,nullptr,nullptr,nullptr)==13);
  if (op==10) { D3DRECT rects[2]{}; clear_rectangles=rects;
                assert(target->lpVtbl->Clear(target,2,rects,7,0x12345678,0.75f,9)==-71); }
 }
 observed_shader_present=d.shader_snapshot!=nullptr;
 if (observed_shader_present) observed_shader=*d.shader_snapshot;
 observed_buffer_observer=d.buffer_observer!=nullptr;
 observed_buffer_observer_valid=d.buffer_observer && d.buffer_observer->valid;
 observed_call=d; events.push_back("capture-draw");
 return next_token++;
}
void CaptureDrawResult(IDirect3DDevice8* d,std::uint64_t token,HRESULT) { SetLastError(991); MockWriterEntry(6,d); if (!token || stress) return; events.push_back("capture-draw-result"); }
void CaptureBufferedDrawResult(IDirect3DDevice8*,std::uint64_t token,HRESULT,bool stable) {
 SetLastError(991); if(!token || stress || observed_abandoned) return;
 observed_buffer_result=true; observed_buffer_stable=stable; events.push_back("capture-draw-result");
}
void CaptureShaderDrawResult(IDirect3DDevice8* d,std::uint64_t token,HRESULT,int buffer_stable,bool shader_stable) {
 SetLastError(991); MockWriterEntry(6,d); if (!token || stress || observed_abandoned) return;
 observed_shader_result=true; observed_shader_stable=shader_stable; observed_shader_buffer_stable=buffer_stable;
 observed_shader_stabilities.push_back(shader_stable);
 events.push_back("capture-draw-result");
}
std::uint64_t CaptureClear(IDirect3DDevice8* d,DWORD count,const D3DRECT* rectangles,DWORD flags,D3DCOLOR color,float depth,DWORD stencil) {
 SetLastError(991); MockWriterEntry(2,d); clear_count=count; clear_rectangles=rectangles; clear_flags=flags; clear_color=color;
 clear_depth=depth; clear_stencil=stencil; events.push_back("capture-clear"); return next_token++;
}
void CaptureClearResult(IDirect3DDevice8* d,std::uint64_t token,HRESULT hr) {
 SetLastError(991); MockWriterEntry(3,d); assert(token && hr==-71); events.push_back("capture-clear-result");
}
void CaptureReset(IDirect3DDevice8* d,const D3DPRESENT_PARAMETERS* p,HRESULT hr) { SetLastError(991); MockWriterEntry(5,d); assert(hr==(declaration_reset_fails ? -81 : 12)); observed_params=const_cast<D3DPRESENT_PARAMETERS*>(p); events.push_back("capture-reset"); }
void CapturePresent(IDirect3DDevice8* d,HRESULT hr) {
 SetLastError(991); MockWriterEntry(4,d); assert(hr==13); events.push_back("capture-present");
 if(stop_on_present) enabled=false;
}
}
static HRESULT query_factory(IDirect3D8* f,REFIID,void** out) { *out=f; ++f->refs; return 0; }
static ULONG add_factory(IDirect3D8* f) { return ++f->refs; }
static ULONG release_factory(IDirect3D8* f) { return --f->refs; }
static HRESULT make_device(IDirect3D8*,UINT,D3DDEVTYPE,HWND,DWORD,D3DPRESENT_PARAMETERS*,IDirect3DDevice8** out) {
 events.push_back("create-device");
 if (create_fails) return -123;
 *out=&devices[next_device]; return 0;
}
static HRESULT query_device(IDirect3DDevice8* d,REFIID,void** out) {
 IDirect3DDevice8* result=device_query_alias ? &devices[query_alias_index] : d;
 *out=result; ++result->refs; return 0;
}
static ULONG add_device(IDirect3DDevice8* d) { return ++d->refs; }
static ULONG release_device(IDirect3DDevice8* d) { ULONG refs=--d->refs; if (!refs) last_freed=d; return refs; }
static HRESULT reset(IDirect3DDevice8*,D3DPRESENT_PARAMETERS*) { reset_input_error=GetLastError(); SetLastError(912); events.push_back("reset"); return declaration_reset_fails ? -81 : 12; }
static HRESULT present(IDirect3DDevice8*,const RECT*,const RECT*,HWND,const RGNDATA*) { present_input_error=GetLastError(); SetLastError(913); events.push_back("present"); return 13; }
static HRESULT clear(IDirect3DDevice8*,DWORD count,const D3DRECT* rectangles,DWORD flags,D3DCOLOR color,float depth,DWORD stencil) {
 clear_input_error=GetLastError(); assert(count==2 && rectangles==clear_rectangles && flags==7 && color==0x12345678 && depth==0.75f && stencil==9);
 SetLastError(914); events.push_back("clear"); return -71;
}
static HRESULT draw(IDirect3DDevice8*,int,UINT,UINT) {
 if (stress) { ++forwarded_draws; return 14; }
 if (declaration_testing) { draw_input_error=GetLastError(); SetLastError(955); }
 if(block_draw) { draw_entered=true; while(!allow_draw) std::this_thread::yield(); }
 events.push_back("draw"); return declaration_draw_fails ? -77 : 14;
}
static HRESULT draw_indexed(IDirect3DDevice8*,int,UINT,UINT,UINT,UINT) { events.push_back("draw-indexed"); return 15; }
static HRESULT draw_up(IDirect3DDevice8*,int,UINT,const void* v,UINT) {
 draw_up_input_error=GetLastError(); SetLastError(909); observed_vertices=v; events.push_back("draw-up");
 if(up_clears_stream && bound_vertex) { bound_vertex->Release(); bound_vertex=nullptr; bound_stride=0; }
 return 16;
}
static HRESULT draw_indexed_up(IDirect3DDevice8*,int,UINT,UINT,UINT,const void* i,int,const void* v,UINT) { observed_indices=i; observed_vertices=v; events.push_back("draw-indexed-up"); return 17; }
static HRESULT create_vertex(IDirect3DDevice8* d,UINT size,DWORD usage,DWORD fvf,D3DPOOL pool,IDirect3DVertexBuffer8** out) {
 vertex_create_error=GetLastError(); SetLastError(941);
 if(FAILED(create_vertex_result)) return create_vertex_result;
 assert(out && size<=128 && !native_vertex); native_vertex=std::make_unique<NativeVertex>(d,size,usage,fvf,pool);
 *out=native_vertex.get(); return S_OK;
}
static HRESULT create_index(IDirect3DDevice8* d,UINT size,DWORD usage,D3DFORMAT format,D3DPOOL pool,IDirect3DIndexBuffer8** out) {
 index_create_error=GetLastError(); SetLastError(942);
 if(FAILED(create_index_result)) return create_index_result;
 assert(out && size<=128 && !native_index); native_index=std::make_unique<NativeIndex>(d,size,usage,format,pool);
 *out=native_index.get(); return S_OK;
}
static HRESULT set_stream(IDirect3DDevice8*,UINT stream,IDirect3DVertexBuffer8* buffer,UINT stride) {
 stream_set_error=GetLastError(); SetLastError(943); assert(stream==0);
 if(block_set_stream) { set_stream_entered=true; while(!allow_set_stream) std::this_thread::yield(); }
 if(FAILED(set_stream_result)) return set_stream_result;
 assert(!buffer || buffer==native_vertex.get());
 if(buffer) buffer->AddRef(); if(bound_vertex) bound_vertex->Release(); bound_vertex=buffer; bound_stride=stride; return S_OK;
}
static HRESULT get_stream(IDirect3DDevice8*,UINT stream,IDirect3DVertexBuffer8** out,UINT* stride) {
 stream_get_error=GetLastError(); SetLastError(944); assert(stream==0);
 if(FAILED(get_stream_result)) return get_stream_result;
 if(!out || !stride) return E_POINTER;
 *out=bound_vertex; *stride=bound_stride; if(*out) (*out)->AddRef();
 if(drop_binding_during_get && bound_vertex) { bound_vertex->Release(); bound_vertex=nullptr; }
 return S_OK;
}
static HRESULT set_indices(IDirect3DDevice8*,IDirect3DIndexBuffer8* buffer,UINT base) {
 indices_set_error=GetLastError(); SetLastError(945);
 if(FAILED(set_indices_result)) return set_indices_result;
 assert(!buffer || buffer==native_index.get());
 if(buffer) buffer->AddRef(); if(bound_index) bound_index->Release(); bound_index=buffer; bound_base=base; return S_OK;
}
static HRESULT get_indices(IDirect3DDevice8*,IDirect3DIndexBuffer8** out,UINT* base) {
 indices_get_error=GetLastError(); SetLastError(946);
 if(FAILED(get_indices_result)) return get_indices_result;
 if(!out || !base) return E_POINTER;
 *out=bound_index; *base=bound_base; if(*out) (*out)->AddRef(); return S_OK;
}
static HRESULT process_vertices(IDirect3DDevice8*,UINT,UINT,UINT,IDirect3DVertexBuffer8* target,DWORD) {
 assert(!target || target==native_vertex.get()); return S_OK;
}
#ifdef T3RENDER_STATEBLOCK_TEST_FIXTURE
static HRESULT begin_state_block(IDirect3DDevice8*) { return stateblock_call(21); }
static HRESULT end_state_block(IDirect3DDevice8*,DWORD* token) { auto hr=stateblock_call(22); if(SUCCEEDED(hr) && token) *token=7; return hr; }
static HRESULT apply_state_block(IDirect3DDevice8*,DWORD) { return stateblock_call(23); }
static HRESULT capture_state_block(IDirect3DDevice8*,DWORD) { return stateblock_call(24); }
static HRESULT delete_state_block(IDirect3DDevice8*,DWORD) { return stateblock_call(25); }
static HRESULT create_state_block(IDirect3DDevice8*,D3DSTATEBLOCKTYPE,DWORD* token) { auto hr=stateblock_call(26); if(SUCCEEDED(hr) && token) *token=8; return hr; }
#else
static HRESULT begin_state_block(IDirect3DDevice8*) { return 21; }
static HRESULT end_state_block(IDirect3DDevice8*,DWORD* token) { if(token) *token=7; return 22; }
static HRESULT apply_state_block(IDirect3DDevice8*,DWORD) { return 23; }
static HRESULT capture_state_block(IDirect3DDevice8*,DWORD) { return 24; }
static HRESULT delete_state_block(IDirect3DDevice8*,DWORD) { return 25; }
static HRESULT create_state_block(IDirect3DDevice8*,D3DSTATEBLOCKTYPE,DWORD* token) { if(token) *token=8; return 26; }
#endif
static HRESULT create_shader(IDirect3DDevice8*,const DWORD*,const DWORD*,DWORD* out,DWORD) {
 shader_input_error=GetLastError(); SetLastError(951);
 if (declaration_create_fails) return -71;
 if (out) *out=shader_handle;
 if (declaration_output_unreadable) unreadable_address=out;
 return S_OK;
}
static HRESULT set_shader(IDirect3DDevice8*,DWORD value) {
 shader_input_error=GetLastError(); SetLastError(952);
 if (declaration_set_fails) return -82;
 shader_value=value; return S_OK;
}
static HRESULT get_shader(IDirect3DDevice8* device,DWORD* out) {
 ++declaration_getter_count;
 const int reentry=shader_getter_reentry;
 if (reentry!=4) shader_getter_reentry=0;
 if (reentry==1) assert(device->lpVtbl->SetVertexShader(device,0xf0000002)==S_OK);
 if (reentry==2 || reentry==4) assert(device->lpVtbl->DrawPrimitive(device,4,0,1)==14);
 if (reentry==3) assert(device->lpVtbl->Release(device)==0);
 SetLastError(953);
 if (declaration_getter_fails) return E_FAIL;
 *out=shader_value; return S_OK;
}
static HRESULT delete_shader(IDirect3DDevice8*,DWORD) { shader_input_error=GetLastError(); SetLastError(954); return declaration_delete_fails ? -83 : S_OK; }
static IDirect3D8Vtbl factory_vt{query_factory,add_factory,release_factory,make_device};
static IDirect3DDevice8Vtbl device_vt{query_device,add_device,release_device,reset,present,clear,draw,draw_indexed,draw_up,draw_indexed_up,
 create_vertex,create_index,set_stream,get_stream,set_indices,get_indices,process_vertices,
 begin_state_block,end_state_block,apply_state_block,capture_state_block,delete_state_block,create_state_block,
 create_shader,set_shader,get_shader,delete_shader};

static int DeclarationHistoryBoundaryTest(const std::string& mode) {
 using t3render::ir::ShaderObservationStatus;
 events.reserve(1024);
 enabled=true; declaration_testing=true; declarations_requested=mode!="hist-without-decl";
 history_requested=mode!="hist-opt-out"; invalid_history_option=mode=="hist-invalid-option";
 buffers_requested=mode=="hist-combined";
 factory={&factory_vt,1}; devices[0]={&device_vt,1}; devices[1]={&device_vt,1};
 auto* f=Proxy_Direct3DCreate8(220); D3DPRESENT_PARAMETERS params{23}; IDirect3DDevice8* d=nullptr;
 assert(f->lpVtbl->CreateDevice(f,0,0,nullptr,0,&params,&d)==S_OK);
 const DWORD words[]={1u<<29,(2u<<29)|(2u<<16),0xffffffffu};
 DWORD handle=0;
 SetLastError(101);
 assert(d->lpVtbl->CreateVertexShader(d,words,nullptr,&handle,0)==S_OK && shader_input_error==101 && GetLastError()==951);
 SetLastError(102);
 assert(d->lpVtbl->SetVertexShader(d,handle)==S_OK && shader_input_error==102 && GetLastError()==952);
 declaration_reset_fails=mode=="hist-failed-reset";
 SetLastError(103);
 assert(d->lpVtbl->Reset(d,&params)==(declaration_reset_fails ? -81 : 12) && reset_input_error==103 && GetLastError()==912);
 if (declaration_reset_fails) {
  declaration_reset_fails=false;
  assert(d->lpVtbl->Reset(d,&params)==12); // recovery cannot revive an unknown failed-reset lifetime
 }
 if (mode=="hist-collision") { DWORD duplicate=0; assert(d->lpVtbl->CreateVertexShader(d,words,nullptr,&duplicate,0)==S_OK); }
 if (mode=="hist-delete-fail") {
  declaration_delete_fails=true;
  SetLastError(104); assert(d->lpVtbl->DeleteVertexShader(d,handle)==-83 && shader_input_error==104 && GetLastError()==954);
  declaration_delete_fails=false;
 }
 if (mode=="hist-stateblock") assert(d->lpVtbl->ApplyStateBlock(d,7)==23);
 if (mode=="hist-alias") { device_query_alias=true; void* alias=nullptr; assert(d->lpVtbl->QueryInterface(d,IID_IDirect3DDevice8,&alias)==S_OK); }
 if (mode!="hist-no-rebind") {
  declaration_set_fails=mode=="hist-failed-rebind";
  SetLastError(105);
  assert(d->lpVtbl->SetVertexShader(d,mode=="hist-other-rebind" ? 0xf0000002u : handle)==
         (declaration_set_fails ? -82 : S_OK));
  assert(shader_input_error==105 && GetLastError()==952);
  declaration_set_fails=false;
 }
 if (mode=="hist-other-rebind") shader_value=handle; // simulate an untracked alias changing the current value
 if (mode=="hist-overlap") {
  block_draw=true; std::thread drawer([&] { assert(d->lpVtbl->DrawPrimitive(d,4,0,1)==14); });
  while (!draw_entered) std::this_thread::yield();
  assert(d->lpVtbl->SetVertexShader(d,handle)==S_OK);
  allow_draw=true; drawer.join(); block_draw=false;
 } else {
  SetLastError(106); assert(d->lpVtbl->DrawPrimitive(d,4,0,1)==14 && draw_input_error==106 && GetLastError()==955);
 }
 if (!declarations_requested || buffers_requested) {
  assert(!observed_shader_present && !observed_shader_result);
  return 0;
 }
 assert(observed_shader_present && observed_shader_result && observed_shader_buffer_stable==-1);
 const bool expected = mode=="hist-on" || mode=="hist-overlap";
 if (expected) {
  assert(observed_shader.declaration.status==ShaderObservationStatus::Unavailable &&
         observed_shader.declaration.reason==t3render::ir::ShaderUnavailableReason::ResetAttempt &&
         observed_shader.declaration.words.empty() &&
         observed_shader.declaration.pre_reset_words==std::vector<std::uint32_t>(std::begin(words),std::end(words)) &&
         observed_shader.declaration.reset_hr==12 && observed_shader.declaration.reset_epoch &&
         observed_shader.declaration.rebind_epoch>observed_shader.declaration.reset_epoch);
 } else assert(observed_shader.declaration.pre_reset_words.empty());
 assert(observed_shader_stable==(mode!="hist-stateblock" && mode!="hist-alias" && mode!="hist-overlap"));
 return 0;
}

static int DeclarationBoundaryTest(const std::string& mode) {
 using t3render::ir::ShaderObservationStatus;
 events.reserve(1024);
 enabled=true; declaration_testing=true; declarations_requested=mode!="decl-opt-out" && mode!="decl-program-without-decl";
 vertex_programs_requested=mode=="decl-program-in" || mode=="decl-program-null" ||
  mode=="decl-program-unsupported" || mode=="decl-program-without-decl";
 buffers_requested=mode=="decl-combined" || mode=="decl-cross-combined-draw";
 capture_device_reentry=mode=="decl-create-callback";
 factory={&factory_vt,1}; devices[0]={&device_vt,1}; devices[1]={&device_vt,1};
 auto* f=Proxy_Direct3DCreate8(220); D3DPRESENT_PARAMETERS params{23}; IDirect3DDevice8* d=nullptr;
 assert(f->lpVtbl->CreateDevice(f,0,0,nullptr,0,&params,&d)==S_OK);
 if (mode=="decl-create-callback") {
  assert(observed_abandoned && !enabled);
  return 0;
 }
 if (mode.find("decl-cross-")==0) {
  device_query_alias=true; void* alias=nullptr;
  assert(d->lpVtbl->QueryInterface(d,IID_IDirect3DDevice8,&alias)==S_OK && alias==&devices[1]);
  device_query_alias=false; shader_writer_target=static_cast<IDirect3DDevice8*>(alias);
 }
 const DWORD words[]={1u<<29,(4u<<29)|(1u<<25),0xffffffffu,2,3,4,
                      (5u<<29)|(2u<<24),0xffffffffu,5,0xffffffffu};
 if (mode.find("decl-entry-")==0) {
  writer_entry_reentry=mode=="decl-entry-config" ? 1 : mode=="decl-entry-clear" ? 2 :
   mode=="decl-entry-clear-result" ? 3 : mode=="decl-entry-present" ? 4 :
   mode=="decl-entry-reset" ? 5 : mode=="decl-entry-draw-result" ? 6 : 8;
  SetLastError(104);
  if (writer_entry_reentry==1) {
   assert(Proxy_Direct3DCreate8(220)==f && GetLastError()==4242);
  } else if (writer_entry_reentry==2 || writer_entry_reentry==3) {
   D3DRECT rects[2]{}; clear_rectangles=rects;
   assert(d->lpVtbl->Clear(d,2,rects,7,0x12345678,0.75f,9)==-71 && GetLastError()==914);
  } else if (writer_entry_reentry==4) {
   assert(d->lpVtbl->Present(d,nullptr,nullptr,nullptr,nullptr)==13 && GetLastError()==913);
  } else if (writer_entry_reentry==5) {
   assert(d->lpVtbl->Reset(d,&params)==12 && GetLastError()==912);
  } else {
   assert(d->lpVtbl->DrawPrimitive(d,4,0,1)==14 && GetLastError()==955);
  }
  assert(!writer_entry_reentry && observed_abandoned && !enabled);
  return 0;
 }
 if (mode=="decl-alias-capacity") {
  // Neither leaked aliases nor poisoned owners can reserve the bounded table.
  for (unsigned i=0; i<10; ++i) {
   IDirect3DDevice8* owner=d;
   if (i) {
    devices[2*i]={&device_vt,1}; next_device=2*i;
    assert(f->lpVtbl->CreateDevice(f,0,0,nullptr,0,&params,&owner)==S_OK);
   }
   const unsigned alias_index=2*i+1;
   devices[alias_index]={&device_vt,1}; query_alias_index=alias_index; device_query_alias=true;
   void* alias=nullptr;
   assert(owner->lpVtbl->QueryInterface(owner,IID_IDirect3DDevice8,&alias)==S_OK && alias==&devices[alias_index]);
   device_query_alias=false;
  }
  devices[20]={&device_vt,1}; next_device=20;
  IDirect3DDevice8* fresh=nullptr;
  assert(f->lpVtbl->CreateDevice(f,0,0,nullptr,0,&params,&fresh)==S_OK);
  DWORD fresh_handle=0;
  assert(fresh->lpVtbl->CreateVertexShader(fresh,words,nullptr,&fresh_handle,0)==S_OK);
  assert(fresh->lpVtbl->SetVertexShader(fresh,fresh_handle)==S_OK);
  assert(fresh->lpVtbl->DrawPrimitive(fresh,4,0,1)==14);
  assert(observed_shader_present && observed_shader_stable &&
         observed_shader.declaration.status==ShaderObservationStatus::Candidate);
  return 0;
 }
 DWORD handle=0;
 declaration_create_fails=mode=="decl-create-fail";
 declaration_output_unreadable=mode=="decl-bad-output";
 const DWORD* input=mode=="decl-incomplete" ? nullptr : words;
 const DWORD program[]={0xfffe0101u,1u,0xc00f0000u,0x90e40000u,0x0000ffffu};
 const DWORD unsupported_program[]={0xfffe0200u};
 const DWORD* function=mode=="decl-program-null" ? nullptr : mode=="decl-program-unsupported" ? unsupported_program :
  mode.find("decl-program-")==0 ? program : nullptr;
 DWORD* output=mode=="decl-null-output" ? nullptr : &handle;
 SetLastError(101);
 auto hr=d->lpVtbl->CreateVertexShader(d,input,function,output,0);
 assert(hr==(declaration_create_fails ? -71 : S_OK) && shader_input_error==101 && GetLastError()==951);
 unreadable_address=nullptr;
 if (declaration_create_fails) handle=shader_handle;
 if (mode=="decl-null-output") handle=shader_handle;
 declaration_create_fails=false; declaration_output_unreadable=false;
 if (mode=="decl-collision") { DWORD second=0; assert(d->lpVtbl->CreateVertexShader(d,words,nullptr,&second,0)==S_OK); }
 if (mode=="decl-reuse") {
  SetLastError(102); assert(d->lpVtbl->DeleteVertexShader(d,handle)==S_OK && shader_input_error==102 && GetLastError()==954);
  assert(d->lpVtbl->CreateVertexShader(d,words,nullptr,&handle,0)==S_OK);
 }
 SetLastError(103); assert(d->lpVtbl->SetVertexShader(d,handle)==S_OK && shader_input_error==103 && GetLastError()==952);
 if (mode=="decl-reset") assert(d->lpVtbl->Reset(d,&params)==12);
 if (mode=="decl-stateblock") assert(d->lpVtbl->ApplyStateBlock(d,7)==23);
 if (mode=="decl-alias") { device_query_alias=true; void* alias=nullptr; assert(d->lpVtbl->QueryInterface(d,IID_IDirect3DDevice8,&alias)==S_OK); }
 wants_draw=mode!="decl-inactive";
 declaration_getter_fails=mode=="decl-get-fail";
 declaration_draw_fails=mode=="decl-draw-fail";
 shader_getter_reentry=mode=="decl-get-set" ? 1 : mode=="decl-get-draw" ? 2 : mode=="decl-get-release" ? 3 :
  mode=="decl-get-recursive" ? 4 : 0;
 shader_writer_reentry=mode=="decl-writer-set" ? 1 : (mode=="decl-writer-draw" || mode=="decl-cross-draw" || mode=="decl-cross-combined-draw") ? 2 :
  mode=="decl-writer-create" ? 3 : mode=="decl-writer-delete" ? 4 : (mode=="decl-writer-reset" || mode=="decl-cross-reset") ? 5 :
  mode=="decl-writer-state" ? 6 : mode=="decl-writer-alias" ? 7 : mode=="decl-writer-release" ? 8 :
  (mode=="decl-writer-present" || mode=="decl-cross-present") ? 9 :
  (mode=="decl-writer-clear" || mode=="decl-cross-clear") ? 10 : 0;
 const bool writer_mutation=mode=="decl-writer-set" || mode=="decl-writer-create" ||
  mode=="decl-writer-delete" || mode=="decl-writer-state" || mode=="decl-writer-alias";
 const bool writer_abandon=mode=="decl-writer-draw" || mode=="decl-writer-reset" ||
  mode=="decl-writer-present" || mode=="decl-writer-clear" || mode.find("decl-cross-")==0;
 if (mode=="decl-draw-set" || mode=="decl-draw-create" || mode=="decl-draw-delete") {
  block_draw=true; std::thread drawer([&] { assert(d->lpVtbl->DrawPrimitive(d,4,0,1)==14); });
  while (!draw_entered) std::this_thread::yield();
  if (mode=="decl-draw-set") assert(d->lpVtbl->SetVertexShader(d,handle)==S_OK);
  if (mode=="decl-draw-create") { DWORD concurrent=0; assert(d->lpVtbl->CreateVertexShader(d,words,nullptr,&concurrent,0)==S_OK); }
  if (mode=="decl-draw-delete") assert(d->lpVtbl->DeleteVertexShader(d,handle)==S_OK);
  allow_draw=true; drawer.join(); block_draw=false;
 } else {
  SetLastError(104);
  assert(d->lpVtbl->DrawPrimitive(d,4,0,1)==((mode=="decl-get-release" || mode=="decl-writer-release") ? D3DERR_INVALIDCALL : declaration_draw_fails ? -77 : 14));
  if (mode=="decl-get-release" || mode=="decl-writer-release") {
   assert(GetLastError()==104 && last_freed==d && declaration_getter_count==1);
   assert(std::find(events.begin(),events.end(),"draw")==events.end());
   assert((mode=="decl-writer-release") == observed_shader_present);
   if (mode=="decl-writer-release") assert(deallocation_outside_writer && !deallocation_hook);
   assert(!observed_shader_result);
   return 0;
  }
  assert(draw_input_error==104 && GetLastError()==955);
 }
 if (writer_abandon) {
  assert(observed_abandoned && !observed_shader_result);
  assert(observed_shader_present==(mode!="decl-cross-combined-draw"));
  return 0;
 }
 if (mode=="decl-combined") assert(observed_buffer_observer && observed_buffer_result && observed_buffer_stable);
 else assert(!observed_buffer_observer && !observed_buffer_result);
 if (mode=="decl-opt-out" || mode=="decl-inactive" || mode=="decl-combined" || mode=="decl-program-without-decl") {
  assert(!observed_shader_present && !observed_shader_result && declaration_getter_count==0);
 } else {
  assert(observed_shader_present && observed_shader_result && observed_shader_buffer_stable==-1);
  assert(declaration_getter_count==1);
  if (mode=="decl-get-draw" || mode=="decl-get-recursive") assert(observed_shader_stabilities==std::vector<bool>({false,false}));
  assert(observed_shader.getter_result==(mode=="decl-get-fail" ? E_FAIL : S_OK));
  if (mode!="decl-get-fail") assert(observed_shader.value==(mode=="decl-get-set" ? 0xf0000002u : handle));
  assert(observed_shader_stable == (!writer_mutation && mode!="decl-get-set" && mode!="decl-get-draw" && mode!="decl-get-recursive" && mode!="decl-draw-set" && mode!="decl-draw-create" && mode!="decl-draw-delete" &&
                                    mode!="decl-get-fail" && mode!="decl-draw-fail" &&
                                    mode!="decl-null-output" && mode!="decl-bad-output" && mode!="decl-alias" && mode!="decl-stateblock"));
  if (writer_mutation || mode.find("decl-program-")==0 || mode=="decl-in" || mode=="decl-reuse" || mode=="decl-draw-set" || mode=="decl-draw-create" ||
      mode=="decl-draw-delete" || mode=="decl-draw-fail") {
   assert(observed_shader.declaration.status==ShaderObservationStatus::Candidate);
   assert(observed_shader.declaration.words==std::vector<std::uint32_t>(std::begin(words),std::end(words)));
   if (mode.find("decl-program-")==0) {
    using t3render::ir::ShaderProgramStatus;
    const auto expected=mode=="decl-program-in" ? ShaderProgramStatus::Complete :
     mode=="decl-program-null" ? ShaderProgramStatus::NullAddress :
     mode=="decl-program-unsupported" ? ShaderProgramStatus::UnsupportedVersion : ShaderProgramStatus::NotRequested;
    assert(observed_shader.declaration.program_status==expected);
    if (expected==ShaderProgramStatus::Complete)
     assert(observed_shader.declaration.program_words==std::vector<std::uint32_t>(std::begin(program),std::end(program)));
    else assert(observed_shader.declaration.program_words.empty());
   }
  } else if (mode=="decl-incomplete" || mode=="decl-collision" || mode=="decl-reset") {
   assert(observed_shader.declaration.status==ShaderObservationStatus::Unavailable &&
          observed_shader.declaration.has_scan_status && observed_shader.declaration.words.empty());
   const auto expected = mode=="decl-incomplete" ? t3render::ir::ShaderUnavailableReason::ScanIncomplete :
      mode=="decl-collision" ? t3render::ir::ShaderUnavailableReason::HandleCollision :
                               t3render::ir::ShaderUnavailableReason::ResetAttempt;
   assert(observed_shader.declaration.reason == expected);
   assert(observed_shader.declaration.scan_status == (mode=="decl-incomplete" ?
          t3render::ir::DeclarationStatus::NullAddress : t3render::ir::DeclarationStatus::Complete));
  } else if (mode=="decl-get-set" || mode=="decl-get-draw" || mode=="decl-get-recursive") {
   assert(observed_shader.declaration.status==ShaderObservationStatus::Unsafe && observed_shader.declaration.words.empty());
  } else if (mode=="decl-null-output" || mode=="decl-bad-output" || mode=="decl-alias" || mode=="decl-stateblock") {
   // An unsafe device may fail BeginDraw; candidate bytes must stay absent.
   assert(observed_shader.declaration.status!=ShaderObservationStatus::Candidate);
  } else assert(observed_shader.declaration.status==ShaderObservationStatus::Unknown);
 }
 return 0;
}

static int BufferBoundaryTest(const std::string& mode) {
 const bool active = mode!="no-capture";
 const bool wrapped = mode!="opt-out" && mode!="invalid-opt-in" && active;
 enabled=active; buffers_requested=(mode!="opt-out");
 invalid_buffer_option=mode=="invalid-opt-in";
 factory={&factory_vt,1}; devices[0]={&device_vt,1}; devices[1]={&device_vt,1};
 IDirect3D8* f=Proxy_Direct3DCreate8(220);
 assert(f==&factory && ((f->lpVtbl!=&factory_vt)==active));
 D3DPRESENT_PARAMETERS params{23}; IDirect3DDevice8* d=nullptr;
 assert(f->lpVtbl->CreateDevice(f,0,0,nullptr,0,&params,&d)==S_OK && d==&devices[0]);

 IDirect3DVertexBuffer8* vertex=nullptr;
 IDirect3DIndexBuffer8* index=nullptr;
 create_vertex_result=-71; SetLastError(11);
 assert(d->lpVtbl->CreateVertexBuffer(d,100,D3DUSAGE_WRITEONLY,0x44,D3DPOOL_MANAGED,&vertex)==-71 && !vertex);
 assert(vertex_create_error==11 && GetLastError()==941); create_vertex_result=0;
 create_index_result=-72; SetLastError(12);
 assert(d->lpVtbl->CreateIndexBuffer(d,10,D3DUSAGE_WRITEONLY,D3DFMT_INDEX16,D3DPOOL_MANAGED,&index)==-72 && !index);
 assert(index_create_error==12 && GetLastError()==942); create_index_result=0;
 SetLastError(13);
 assert(d->lpVtbl->CreateVertexBuffer(d,100,D3DUSAGE_WRITEONLY,0x44,D3DPOOL_MANAGED,&vertex)==S_OK && vertex);
 assert(vertex_create_error==13 && GetLastError()==941);
 SetLastError(14);
 assert(d->lpVtbl->CreateIndexBuffer(d,10,D3DUSAGE_WRITEONLY,D3DFMT_INDEX16,D3DPOOL_MANAGED,&index)==S_OK && index);
 assert(index_create_error==14 && GetLastError()==942);
 assert((vertex!=native_vertex.get())==wrapped && (index!=native_index.get())==wrapped);

 BYTE* data=nullptr; SetLastError(15);
 assert(vertex->Lock(20,80,&data,0)==S_OK && data==native_vertex->bytes.data()+20);
 assert(native_vertex->lock_input_error==15 && GetLastError()==931);
 for(unsigned i=0;i<80;++i) data[i]=static_cast<BYTE>(i);
 SetLastError(16); assert(vertex->Unlock()==S_OK);
 assert(native_vertex->unlock_input_error==16 && GetLastError()==932);
 SetLastError(17); assert(index->Lock(4,6,&data,0)==S_OK && data==native_index->bytes.data()+4);
 for(unsigned i=0;i<6;++i) data[i]=static_cast<BYTE>(i+3);
 SetLastError(18); assert(index->Unlock()==S_OK);
 assert(native_index->unlock_input_error==18 && GetLastError()==932);

#ifdef T3RENDER_PROXY_TEST
 if(wrapped) {
  std::array<BYTE,80> observed_vertex{};
  std::array<BYTE,6> observed_index{};
  assert(T3ProxyTestReadVertex(d,native_vertex.get(),20,observed_vertex.data(),observed_vertex.size())==t3render::ir::RegistryResult::Ok);
  assert(T3ProxyTestReadIndex(d,native_index.get(),4,observed_index.data(),observed_index.size())==t3render::ir::RegistryResult::Ok);
  for(unsigned i=0;i<80;++i) assert(observed_vertex[i]==i);
  for(unsigned i=0;i<6;++i) assert(observed_index[i]==i+3);
  BYTE untouched=0xee;
  assert(T3ProxyTestReadVertex(d,native_vertex.get(),0,&untouched,1)==t3render::ir::RegistryResult::UnknownData && untouched==0xee);
  if(mode=="device-query-alias") {
   // A distinct QI alias may receive calls outside the original device
   // observer. Both the owner and alias must remain untrusted thereafter.
   device_query_alias=true; void* queried=nullptr;
   assert(d->lpVtbl->QueryInterface(d,IID_IDirect3DDevice8,&queried)==S_OK && queried==&devices[1]);
   auto* alias=static_cast<IDirect3DDevice8*>(queried);
   assert(T3ProxyTestReadVertex(d,native_vertex.get(),20,observed_vertex.data(),observed_vertex.size())!=t3render::ir::RegistryResult::Ok);
   assert(T3ProxyTestReadIndex(d,native_index.get(),4,observed_index.data(),observed_index.size())!=t3render::ir::RegistryResult::Ok);
   vertex->Release(); index->Release(); assert(native_vertex->destroyed && native_index->destroyed);
   native_vertex.reset(); native_index.reset();
   IDirect3DVertexBuffer8* later=nullptr;
   assert(alias->lpVtbl->CreateVertexBuffer(alias,100,D3DUSAGE_WRITEONLY,0x44,D3DPOOL_MANAGED,&later)==S_OK && later);
   BYTE* later_data=nullptr; assert(later->Lock(20,80,&later_data,0)==S_OK && later_data);
   std::memset(later_data,1,80); assert(later->Unlock()==S_OK);
   assert(T3ProxyTestReadVertex(alias,native_vertex.get(),20,observed_vertex.data(),observed_vertex.size())!=t3render::ir::RegistryResult::Ok);
   later->Release(); assert(native_vertex->destroyed);
   assert(alias->lpVtbl->Release(alias)==1); assert(d->lpVtbl->Release(d)==0); assert(f->lpVtbl->Release(f)==0);
   std::cout<<"buffer boundary "<<mode<<": passed\n"; return 0;
  }
  if(mode=="alias" || mode=="stateblock" || mode=="reset" || mode=="device-alias" ||
     mode=="raw-vertex" || mode=="raw-index") {
   if(mode=="alias") {
    void* raw_alias=nullptr;
    const IID unknown{99};
    assert(vertex->QueryInterface(unknown,&raw_alias)==S_OK && raw_alias==native_vertex.get());
    static_cast<IDirect3DVertexBuffer8*>(raw_alias)->Release();
   } else if(mode=="device-alias") {
    IDirect3DDevice8* raw_device=nullptr;
    return_device_alias=true;
    assert(vertex->GetDevice(&raw_device)==S_OK && raw_device==&devices[1]);
    raw_device->lpVtbl->Release(raw_device);
   } else if(mode=="stateblock") {
    DWORD token=0;
    assert(d->lpVtbl->BeginStateBlock(d)==21);
    assert(d->lpVtbl->EndStateBlock(d,&token)==22 && token==7);
    assert(d->lpVtbl->CreateStateBlock(d,0,&token)==26 && token==8);
    assert(d->lpVtbl->CaptureStateBlock(d,token)==24);
    assert(d->lpVtbl->ApplyStateBlock(d,token)==23);
    assert(d->lpVtbl->DeleteStateBlock(d,token)==25);
   } else if(mode=="raw-vertex") {
    assert(d->lpVtbl->SetStreamSource(d,0,native_vertex.get(),20)==S_OK);
    assert(d->lpVtbl->SetStreamSource(d,0,nullptr,0)==S_OK);
   } else if(mode=="raw-index") {
    assert(d->lpVtbl->SetIndices(d,native_index.get(),1)==S_OK);
    assert(d->lpVtbl->SetIndices(d,nullptr,0)==S_OK);
   } else {
    assert(d->lpVtbl->Reset(d,&params)==12);
   }
   // An escaped alias or state mutation invalidates the entire device, never
   // just the resource through which it was observed.
   assert(T3ProxyTestReadVertex(d,native_vertex.get(),20,observed_vertex.data(),observed_vertex.size())!=t3render::ir::RegistryResult::Ok);
   assert(T3ProxyTestReadIndex(d,native_index.get(),4,observed_index.data(),observed_index.size())!=t3render::ir::RegistryResult::Ok);
   vertex->Release(); index->Release();
   assert(native_vertex->destroyed && native_index->destroyed);
   assert(d->lpVtbl->Release(d)==0); assert(f->lpVtbl->Release(f)==0);
   std::cout<<"buffer boundary "<<mode<<": passed\n";
   return 0;
  }
 }
#endif

 set_stream_result=-73; SetLastError(19);
 assert(d->lpVtbl->SetStreamSource(d,0,vertex,20)==-73 && !bound_vertex);
 assert(stream_set_error==19 && GetLastError()==943); set_stream_result=0;
 set_indices_result=-74; SetLastError(20);
 assert(d->lpVtbl->SetIndices(d,index,1)==-74 && !bound_index);
 assert(indices_set_error==20 && GetLastError()==945); set_indices_result=0;
 assert(d->lpVtbl->SetStreamSource(d,0,vertex,20)==S_OK && bound_vertex==native_vertex.get());
 assert(d->lpVtbl->SetIndices(d,index,1)==S_OK && bound_index==native_index.get());

 IDirect3DVertexBuffer8* got_vertex=nullptr; IDirect3DIndexBuffer8* got_index=nullptr;
 UINT stride=0,base=0; get_stream_result=-75; SetLastError(21);
 assert(d->lpVtbl->GetStreamSource(d,0,&got_vertex,&stride)==-75 && !got_vertex);
 assert(stream_get_error==21 && GetLastError()==944); get_stream_result=0;
 get_indices_result=-76; SetLastError(22);
 assert(d->lpVtbl->GetIndices(d,&got_index,&base)==-76 && !got_index);
 assert(indices_get_error==22 && GetLastError()==946); get_indices_result=0;
 SetLastError(23); assert(d->lpVtbl->GetStreamSource(d,0,&got_vertex,&stride)==S_OK);
 assert(got_vertex==vertex && stride==20 && stream_get_error==23 && GetLastError()==944);
 SetLastError(24); assert(d->lpVtbl->GetIndices(d,&got_index,&base)==S_OK);
 assert(got_index==index && base==1 && indices_get_error==24 && GetLastError()==946);
 got_vertex->Release(); got_index->Release();
#if defined(T3RENDER_PROXY_TEST) && defined(T3RENDER_STATEBLOCK_TEST_FIXTURE)
 if(mode.rfind("state-exp-",0)==0) {
  BYTE sample=0;
  auto known=[&] {
   std::array<BYTE,80> vb{}; std::array<BYTE,6> ib{};
   assert(T3ProxyTestReadVertex(d,native_vertex.get(),20,vb.data(),vb.size())==t3render::ir::RegistryResult::Ok);
   assert(T3ProxyTestReadIndex(d,native_index.get(),4,ib.data(),ib.size())==t3render::ir::RegistryResult::Ok);
   for(unsigned i=0;i<vb.size();++i) assert(vb[i]==i);
   for(unsigned i=0;i<ib.size();++i) assert(ib[i]==i+3);
  };
  auto unavailable=[&] {
   assert(T3ProxyTestReadVertex(d,native_vertex.get(),20,&sample,1)==t3render::ir::RegistryResult::Unavailable);
   assert(T3ProxyTestReadIndex(d,native_index.get(),4,&sample,1)==t3render::ir::RegistryResult::Unavailable);
  };
  auto finish=[&] {
   assert(d->lpVtbl->SetStreamSource(d,0,nullptr,0)==S_OK);
   assert(d->lpVtbl->SetIndices(d,nullptr,0)==S_OK);
   vertex->Release(); index->Release();
   assert(native_vertex->destroyed && native_index->destroyed);
   assert(d->lpVtbl->Release(d)==0); assert(f->lpVtbl->Release(f)==0);
  };
  known();
  if(mode=="state-exp-preserve" || mode=="state-exp-failure" || mode=="state-exp-lifetime") {
   fail_stateblock=mode=="state-exp-failure";
   DWORD token=0xabcdef;
   SetLastError(71);
   assert(d->lpVtbl->BeginStateBlock(d)==(fail_stateblock ? -98 : 21));
   assert(stateblock_input_error==71 && GetLastError()==947); known();
   SetLastError(72);
   assert(d->lpVtbl->EndStateBlock(d,&token)==(fail_stateblock ? -98 : 22));
   assert(stateblock_input_error==72 && GetLastError()==947 && token==(fail_stateblock ? 0xabcdefu : 7u)); known();
   SetLastError(73);
   assert(d->lpVtbl->CreateStateBlock(d,0,&token)==(fail_stateblock ? -98 : 26));
   assert(stateblock_input_error==73 && GetLastError()==947 && token==(fail_stateblock ? 0xabcdefu : 8u)); known();
   SetLastError(74);
   assert(d->lpVtbl->CaptureStateBlock(d,token)==(fail_stateblock ? -98 : 24));
   assert(stateblock_input_error==74 && GetLastError()==947); known();
   SetLastError(75);
   assert(d->lpVtbl->ApplyStateBlock(d,token)==(fail_stateblock ? -98 : 23));
   assert(stateblock_input_error==75 && GetLastError()==947); known();
   SetLastError(76);
   assert(d->lpVtbl->DeleteStateBlock(d,token)==(fail_stateblock ? -98 : 25));
   assert(stateblock_input_error==76 && GetLastError()==947); known();
   got_vertex=nullptr; got_index=nullptr;
   assert(d->lpVtbl->GetStreamSource(d,0,&got_vertex,&stride)==S_OK && got_vertex==vertex && stride==20);
   assert(d->lpVtbl->GetIndices(d,&got_index,&base)==S_OK && got_index==index && base==1);
   got_vertex->Release(); got_index->Release(); known();
   if(mode=="state-exp-lifetime") {
    // Native device bindings outlive both wrappers. Reacquisition is a raw
    // fallback: it must never be advertised as known shadow evidence.
    vertex->Release(); index->Release(); vertex=nullptr; index=nullptr;
    assert(!native_vertex->destroyed && !native_index->destroyed);
    got_vertex=nullptr; got_index=nullptr;
    assert(d->lpVtbl->GetStreamSource(d,0,&got_vertex,&stride)==S_OK && got_vertex==native_vertex.get());
    assert(d->lpVtbl->GetIndices(d,&got_index,&base)==S_OK && got_index==native_index.get());
    unavailable(); got_vertex->Release(); got_index->Release();
    assert(d->lpVtbl->SetStreamSource(d,0,nullptr,0)==S_OK);
    assert(d->lpVtbl->SetIndices(d,nullptr,0)==S_OK);
    assert(native_vertex->destroyed && native_index->destroyed);
    assert(d->lpVtbl->Release(d)==0); assert(f->lpVtbl->Release(f)==0);
   } else finish();
  } else if(mode=="state-exp-state-draw" || mode=="state-exp-state-fail-draw") {
   fail_stateblock=mode=="state-exp-state-fail-draw";
   block_stateblock=true;
   std::thread setter([&] { assert(d->lpVtbl->ApplyStateBlock(d,8)==(fail_stateblock ? -98 : 23)); });
   while(!stateblock_entered) std::this_thread::yield();
   assert(d->lpVtbl->DrawPrimitive(d,4,0,1)==14);
   allow_stateblock=true; setter.join(); block_stateblock=false;
   assert(observed_buffer_observer && !observed_buffer_observer_valid && observed_buffer_result && !observed_buffer_stable);
   unavailable(); finish();
  } else if(mode=="state-exp-draw-state" || mode=="state-exp-draw-state-fail") {
   fail_stateblock=mode=="state-exp-draw-state-fail";
   block_draw=true;
   std::thread drawer([&] { assert(d->lpVtbl->DrawPrimitive(d,4,0,1)==14); });
   while(!draw_entered) std::this_thread::yield();
   assert(d->lpVtbl->ApplyStateBlock(d,8)==(fail_stateblock ? -98 : 23));
   allow_draw=true; drawer.join(); block_draw=false;
   assert(observed_buffer_observer && observed_buffer_observer_valid && observed_buffer_result && !observed_buffer_stable);
   unavailable(); finish();
  } else if(mode=="state-exp-state-lock") {
   block_stateblock=true;
   std::thread setter([&] { assert(d->lpVtbl->ApplyStateBlock(d,8)==23); });
   while(!stateblock_entered) std::this_thread::yield();
   BYTE* mapped=nullptr;
   assert(vertex->Lock(20,4,&mapped,0)==S_OK && mapped);
   std::memset(mapped,0x9a,4); assert(vertex->Unlock()==S_OK);
   allow_stateblock=true; setter.join(); block_stateblock=false;
   assert(d->lpVtbl->DrawPrimitive(d,4,0,1)==14);
   assert(observed_buffer_observer && !observed_buffer_observer_valid && observed_buffer_result && !observed_buffer_stable);
   unavailable(); finish();
  } else if(mode=="state-exp-lock-state") {
   // A lock held before the state-block call invalidates the device too.
   // The native calls and their last-error values must still be forwarded.
   BYTE* mapped=nullptr;
   SetLastError(77);
   assert(vertex->Lock(20,4,&mapped,0)==S_OK && mapped);
   assert(native_vertex->lock_input_error==77 && GetLastError()==931);
   SetLastError(78);
   assert(d->lpVtbl->ApplyStateBlock(d,8)==23);
   assert(stateblock_input_error==78 && GetLastError()==947);
   unavailable();
   SetLastError(79);
   std::memset(mapped,0x9a,4); assert(vertex->Unlock()==S_OK);
   assert(native_vertex->unlock_input_error==79 && GetLastError()==932);
   unavailable();
   assert(d->lpVtbl->DrawPrimitive(d,4,0,1)==14);
   assert(observed_buffer_observer && !observed_buffer_observer_valid && observed_buffer_result && !observed_buffer_stable);
   finish();
  } else assert(false && "unknown state-block experiment mode");
  std::cout<<"buffer boundary "<<mode<<": passed\n"; return 0;
 }
#endif
#ifdef T3RENDER_PROXY_TEST
 if(mode=="stop-present" || mode=="stop-error") {
  BYTE sample=0;
  assert(T3ProxyTestReadVertex(d,native_vertex.get(),20,&sample,1)==t3render::ir::RegistryResult::Ok);
  assert(T3ProxyTestReadIndex(d,native_index.get(),4,&sample,1)==t3render::ir::RegistryResult::Ok);
  if(mode=="stop-present") {
   stop_on_present=true; SetLastError(25);
   assert(d->lpVtbl->Present(d,nullptr,nullptr,nullptr,nullptr)==13 && GetLastError()==913);
  } else {
   throw_capture=true; stop_on_capture_error=true;
   assert(d->lpVtbl->DrawPrimitive(d,4,0,1)==14);
   throw_capture=false;
  }
  assert(T3ProxyTestReadVertex(d,native_vertex.get(),20,&sample,1)!=t3render::ir::RegistryResult::Ok);
  assert(T3ProxyTestReadIndex(d,native_index.get(),4,&sample,1)!=t3render::ir::RegistryResult::Ok);
  BYTE* later=nullptr;
  assert(vertex->Lock(20,4,&later,0)==S_OK && later); std::memset(later,0x44,4);
  assert(vertex->Unlock()==S_OK);
  assert(T3ProxyTestReadVertex(d,native_vertex.get(),20,&sample,1)!=t3render::ir::RegistryResult::Ok);
  // The wrappers remain valid COM objects and still unwrap at the device boundary.
  assert(d->lpVtbl->SetStreamSource(d,0,vertex,20)==S_OK && bound_vertex==native_vertex.get());
  assert(d->lpVtbl->SetIndices(d,index,1)==S_OK && bound_index==native_index.get());
  got_vertex=nullptr; got_index=nullptr;
  assert(d->lpVtbl->GetStreamSource(d,0,&got_vertex,&stride)==S_OK && got_vertex==vertex);
  assert(d->lpVtbl->GetIndices(d,&got_index,&base)==S_OK && got_index==index);
  got_vertex->Release(); got_index->Release();
  assert(d->lpVtbl->SetStreamSource(d,0,nullptr,0)==S_OK);
  assert(d->lpVtbl->SetIndices(d,nullptr,0)==S_OK);
  vertex->Release(); index->Release();
  assert(native_vertex->destroyed && native_index->destroyed);
  assert(d->lpVtbl->Release(d)==0); assert(f->lpVtbl->Release(f)==0);
  std::cout<<"buffer boundary "<<mode<<": passed\n"; return 0;
 }
#endif
 if(mode=="opt-in" || mode=="opt-out" || mode=="invalid-opt-in" || mode=="no-capture") {
  observed_buffer_observer=false; observed_buffer_result=false; observed_buffer_stable=false;
  assert(d->lpVtbl->DrawPrimitive(d,4,0,1)==14);
  assert(observed_buffer_observer==wrapped && observed_buffer_result==wrapped);
  if(wrapped) assert(observed_buffer_observer_valid && observed_buffer_stable);
 }
#ifdef T3RENDER_PROXY_TEST
 if(mode=="set-draw" || mode=="draw-set") {
  BYTE sample=0;
  // Ordinary sequential binds must not discard otherwise known data.
  assert(T3ProxyTestReadVertex(d,native_vertex.get(),20,&sample,1)==t3render::ir::RegistryResult::Ok);
  assert(T3ProxyTestReadIndex(d,native_index.get(),4,&sample,1)==t3render::ir::RegistryResult::Ok);
  if(mode=="set-draw") {
   block_set_stream=true;
   std::thread setter([&] { assert(d->lpVtbl->SetStreamSource(d,0,vertex,20)==S_OK); });
   while(!set_stream_entered) std::this_thread::yield();
   assert(d->lpVtbl->DrawPrimitive(d,4,0,1)==14);
   allow_set_stream=true; setter.join(); block_set_stream=false;
  } else {
   block_draw=true;
   std::thread drawer([&] { assert(d->lpVtbl->DrawPrimitive(d,4,0,1)==14); });
   while(!draw_entered) std::this_thread::yield();
   assert(d->lpVtbl->SetStreamSource(d,0,vertex,20)==S_OK);
   allow_draw=true; drawer.join(); block_draw=false;
  }
  assert(observed_buffer_observer && observed_buffer_result && !observed_buffer_stable);
  assert(observed_buffer_observer_valid==(mode=="draw-set"));
  assert(T3ProxyTestReadVertex(d,native_vertex.get(),20,&sample,1)!=t3render::ir::RegistryResult::Ok);
  assert(T3ProxyTestReadIndex(d,native_index.get(),4,&sample,1)!=t3render::ir::RegistryResult::Ok);
  assert(d->lpVtbl->SetStreamSource(d,0,nullptr,0)==S_OK);
  assert(d->lpVtbl->SetIndices(d,nullptr,0)==S_OK);
  vertex->Release(); index->Release();
  assert(native_vertex->destroyed && native_index->destroyed);
  assert(d->lpVtbl->Release(d)==0); assert(f->lpVtbl->Release(f)==0);
  std::cout<<"buffer boundary "<<mode<<": passed\n"; return 0;
 }
#endif
 if(mode=="up-clears-stream") {
  // D3D8 DrawPrimitiveUP leaves stream 0 unbound. The next getter must
  // observe the runtime's null binding rather than an adapter cache.
  std::array<BYTE,60> immediate{}; up_clears_stream=true;
  observed_buffer_observer=false; observed_buffer_result=false;
  assert(d->lpVtbl->DrawPrimitiveUP(d,4,1,immediate.data(),20)==16);
  assert(!observed_buffer_observer && !observed_buffer_result);
  got_vertex=reinterpret_cast<IDirect3DVertexBuffer8*>(1); stride=99;
  assert(d->lpVtbl->GetStreamSource(d,0,&got_vertex,&stride)==S_OK && !got_vertex && stride==0);
  got_index=nullptr; assert(d->lpVtbl->GetIndices(d,&got_index,&base)==S_OK && got_index==index && base==1);
  got_index->Release();
  assert(d->lpVtbl->SetIndices(d,nullptr,0)==S_OK);
  vertex->Release(); index->Release();
  assert(native_vertex->destroyed && native_index->destroyed);
  assert(d->lpVtbl->Release(d)==0); assert(f->lpVtbl->Release(f)==0);
  std::cout<<"buffer boundary "<<mode<<": passed\n"; return 0;
 }
 assert(d->lpVtbl->ProcessVertices(d,0,0,1,vertex,0)==S_OK);
#ifdef T3RENDER_PROXY_TEST
 if(wrapped) {
  BYTE observed=0;
  assert(T3ProxyTestReadVertex(d,native_vertex.get(),20,&observed,1)!=t3render::ir::RegistryResult::Ok);
 }
#endif

 if(mode=="reentrant") {
  // Last external wrappers go away; the native device still owns its binding.
  assert(d->lpVtbl->SetIndices(d,nullptr,0)==S_OK);
  index->Release(); index=nullptr; vertex->Release(); vertex=nullptr;
  // Leave the native resource's device reference as the last device reference.
  // Getter simulates an unbind racing the internal capture getter, so its
  // temporary reference can be the last resource reference on Release.
  assert(d->lpVtbl->Release(d)==1);
  reentrant_capture=true; drop_binding_during_get=true; events.clear();
  assert(d->lpVtbl->DrawPrimitive(d,4,0,1)==D3DERR_INVALIDCALL);
  assert(last_freed==d && native_vertex->destroyed && !bound_vertex);
  assert(events==std::vector<std::string>({"capture-draw","capture-release"}));
  reentrant_capture=false;
 } else {
  if(wrapped) {
   void* unknown=nullptr;
   assert(vertex->QueryInterface(IID_IUnknown,&unknown)==S_OK && unknown==vertex);
   static_cast<IDirect3DVertexBuffer8*>(unknown)->Release();
   assert(index->QueryInterface(IID_IUnknown,&unknown)==S_OK && unknown==index);
   static_cast<IDirect3DIndexBuffer8*>(unknown)->Release();
  }
  vertex->Release(); index->Release(); vertex=nullptr; index=nullptr;
  got_vertex=nullptr; got_index=nullptr;
  assert(d->lpVtbl->GetStreamSource(d,0,&got_vertex,&stride)==S_OK && got_vertex);
  assert(d->lpVtbl->GetIndices(d,&got_index,&base)==S_OK && got_index);
#ifdef T3RENDER_PROXY_TEST
  if(wrapped) {
   // A weak registry may return a new wrapper or raw fallback. In either case,
   // previously observed bytes are not evidence after the final wrapper release.
   BYTE observed=0;
   assert(T3ProxyTestReadVertex(d,native_vertex.get(),20,&observed,1)!=t3render::ir::RegistryResult::Ok);
   assert(T3ProxyTestReadIndex(d,native_index.get(),4,&observed,1)!=t3render::ir::RegistryResult::Ok);
  }
#endif
  got_vertex->Release(); got_index->Release();
  assert(d->lpVtbl->SetStreamSource(d,0,nullptr,0)==S_OK);
  assert(d->lpVtbl->SetIndices(d,nullptr,0)==S_OK);
  assert(native_vertex->destroyed && native_index->destroyed);
  assert(d->lpVtbl->Release(d)==0);
 }
 assert(f->lpVtbl->Release(f)==0);
 std::cout<<"buffer boundary "<<mode<<": passed\n";
 return 0;
}

int main(int argc, char** argv) {
 if(argc==2 && std::string(argv[1]).find("hist-")==0) return DeclarationHistoryBoundaryTest(argv[1]);
 if(argc==2 && std::string(argv[1]).find("decl-")==0) return DeclarationBoundaryTest(argv[1]);
 if(argc==2) return BufferBoundaryTest(argv[1]);
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
 assert(events==std::vector<std::string>({"capture-draw","draw","capture-draw-result"})); events.clear();
 throw_capture=true; assert(d->lpVtbl->DrawPrimitive(d,4,8,9)==14); throw_capture=false;
 assert(events==std::vector<std::string>({"draw"})); events.clear();
 stress=true;
 std::vector<std::thread> workers;
 for (int n=0; n<4; ++n) workers.emplace_back([d] { for (int i=0; i<200; ++i) assert(d->lpVtbl->DrawPrimitive(d,4,8,9)==14); });
 for (auto& worker : workers) worker.join();
 stress=false; assert(captured_draws==800 && forwarded_draws==800);
 assert(d->lpVtbl->DrawIndexedPrimitive(d,4,5,6,7,8)==15); assert(observed_call.kind==t3render::DrawKind::IndexedPrimitive && observed_call.min_vertex_index==5 && observed_call.num_vertices==6 && observed_call.start_index==7 && observed_call.primitive_count==8);
 assert(events==std::vector<std::string>({"capture-draw","draw-indexed","capture-draw-result"})); events.clear();
 int vertices=1, indices=2;
 SetLastError(123);
 assert(d->lpVtbl->DrawPrimitiveUP(d,4,5,&vertices,24)==16); assert(observed_vertices==&vertices && observed_call.vertex_data==&vertices && !observed_call.index_data && observed_call.kind==t3render::DrawKind::PrimitiveUP && observed_call.vertex_stride==24);
 assert(draw_up_input_error==123 && GetLastError()==909);
 assert(events==std::vector<std::string>({"capture-draw","draw-up","capture-draw-result"})); events.clear();
 assert(d->lpVtbl->DrawIndexedPrimitiveUP(d,4,5,6,7,&indices,9,&vertices,24)==17); assert(observed_indices==&indices && observed_vertices==&vertices && observed_call.vertex_data==&vertices && observed_call.index_data==&indices && observed_call.kind==t3render::DrawKind::IndexedPrimitiveUP && observed_call.index_format==9);
 assert(events==std::vector<std::string>({"capture-draw","draw-indexed-up","capture-draw-result"})); events.clear();
 D3DRECT rects[2]{};
 SetLastError(104);
 assert(d->lpVtbl->Clear(d,2,rects,7,0x12345678,0.75f,9)==-71);
 assert(clear_input_error==104 && GetLastError()==914 && clear_count==2 && clear_rectangles==rects);
 assert(clear_flags==7 && clear_color==0x12345678 && clear_depth==0.75f && clear_stencil==9);
 assert(events==std::vector<std::string>({"capture-clear","clear","capture-clear-result"})); events.clear();
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
