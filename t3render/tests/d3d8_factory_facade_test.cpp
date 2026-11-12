#include "d3d8_factory_facade.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <thread>
#include <vector>

namespace {
using t3render::fixture::FactoryFacade;
constexpr GUID kUnknownIid{99, 0, 0, {}};

struct NativeFactory final : IDirect3D8 {
    ULONG refs = 1;
    unsigned adds = 0, releases = 0, queries = 0, device_calls = 0, last_call = 0;
    DWORD release_input_error = 0;
    std::array<std::uintptr_t, 6> args{};

    void Record(unsigned call, std::initializer_list<std::uintptr_t> values = {}) {
        last_call = call;
        args.fill(0);
        unsigned i = 0;
        for (auto value : values) args[i++] = value;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** output) override {
        ++queries;
        if (!output) return E_POINTER;
        *output = static_cast<IDirect3D8*>(this);
        AddRef();
        return S_OK; // even unsupported native IIDs must not escape through the facade
    }
    ULONG STDMETHODCALLTYPE AddRef() override { ++adds; return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ++releases;
        release_input_error = GetLastError();
        SetLastError(88);
        return --refs;
    }
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* callback) override {
        Record(1, {reinterpret_cast<std::uintptr_t>(callback)}); return 101;
    }
    UINT STDMETHODCALLTYPE GetAdapterCount() override { Record(2); return 102; }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT a, DWORD flags, D3DADAPTER_IDENTIFIER8* out) override {
        Record(3, {a, flags, reinterpret_cast<std::uintptr_t>(out)}); out->value = 103; return 103;
    }
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT a) override { Record(4, {a}); return 104; }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT a, UINT mode, D3DDISPLAYMODE* out) override {
        Record(5, {a, mode, reinterpret_cast<std::uintptr_t>(out)}); out->value = 105; return 105;
    }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT a, D3DDISPLAYMODE* out) override {
        Record(6, {a, reinterpret_cast<std::uintptr_t>(out)}); out->value = 106; return 106;
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT a, D3DDEVTYPE type, D3DFORMAT f, D3DFORMAT b, BOOL w) override {
        Record(7, {a, type, f, b, static_cast<std::uintptr_t>(w)}); return 107;
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT a, D3DDEVTYPE type, D3DFORMAT f, DWORD usage,
                                                D3DRESOURCETYPE resource, D3DFORMAT format) override {
        Record(8, {a, type, f, usage, resource, format}); return 108;
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT a, D3DDEVTYPE type, D3DFORMAT f, BOOL windowed,
                                                         D3DMULTISAMPLE_TYPE sample) override {
        Record(9, {a, type, f, static_cast<std::uintptr_t>(windowed), sample}); return 109;
    }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT a, D3DDEVTYPE type, D3DFORMAT f, D3DFORMAT target,
                                                     D3DFORMAT depth) override {
        Record(10, {a, type, f, target, depth}); return 110;
    }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT a, D3DDEVTYPE type, D3DCAPS8* out) override {
        Record(11, {a, type, reinterpret_cast<std::uintptr_t>(out)}); out->value = 111; return 111;
    }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT a) override {
        Record(12, {a}); return reinterpret_cast<HMONITOR>(0x1234);
    }
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                            IDirect3DDevice8**) override { ++device_calls; return S_OK; }
};

void FactoryComIdentityAndLifetime() {
    NativeFactory native;
    SetLastError(37);
    assert(!FactoryFacade::Adopt(nullptr) && GetLastError() == 37);
    IDirect3D8* factory = FactoryFacade::Adopt(&native);
    assert(factory && GetLastError() == 37 && native.refs == 1 && native.adds == 0);
    void* unknown = nullptr;
    void* factory_iid = nullptr;
    assert(factory->QueryInterface(IID_IUnknown, &unknown) == S_OK && unknown == factory);
    auto* as_unknown = static_cast<IUnknown*>(unknown);
    assert(as_unknown->QueryInterface(IID_IDirect3D8, &factory_iid) == S_OK && factory_iid == factory);
    void* unsupported = reinterpret_cast<void*>(0x1);
    assert(factory->QueryInterface(kUnknownIid, &unsupported) == E_NOINTERFACE && !unsupported);
    assert(factory->QueryInterface(IID_IUnknown, nullptr) == E_POINTER);
    assert(native.queries == 0 && native.adds == 0);
    assert(static_cast<IDirect3D8*>(factory_iid)->Release() == 2);
    assert(as_unknown->Release() == 1);

    // The anchor reference remains held while threads race AddRef/Release.
    // Concurrent use after the caller's final COM Release is not valid COM.
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 8; ++i) threads.emplace_back([&] {
        for (unsigned n = 0; n < 5000; ++n) { assert(factory->AddRef() >= 2); factory->Release(); }
    });
    for (auto& thread : threads) thread.join();
    assert(native.refs == 1 && native.releases == 0);
    SetLastError(37);
    assert(factory->Release() == 0);
    assert(native.refs == 0 && native.releases == 1 && native.release_input_error == 37 && GetLastError() == 88);
}

void FactoryMethodsForwardAndCreationFailsClosed() {
    NativeFactory native;
    IDirect3D8* factory = FactoryFacade::Adopt(&native);
    assert(factory);
    auto* callback = reinterpret_cast<void*>(0x321);
    assert(factory->RegisterSoftwareDevice(callback) == 101 && native.last_call == 1 && native.args[0] == 0x321);
    assert(factory->GetAdapterCount() == 102 && native.last_call == 2);
    D3DADAPTER_IDENTIFIER8 identifier{};
    assert(factory->GetAdapterIdentifier(3, 4, &identifier) == 103 && identifier.value == 103);
    assert(native.last_call == 3 && native.args[0] == 3 && native.args[1] == 4 &&
           native.args[2] == reinterpret_cast<std::uintptr_t>(&identifier));
    assert(factory->GetAdapterModeCount(5) == 104 && native.last_call == 4 && native.args[0] == 5);
    D3DDISPLAYMODE display{};
    assert(factory->EnumAdapterModes(6, 7, &display) == 105 && display.value == 105);
    assert(native.last_call == 5 && native.args[0] == 6 && native.args[1] == 7 &&
           native.args[2] == reinterpret_cast<std::uintptr_t>(&display));
    assert(factory->GetAdapterDisplayMode(8, &display) == 106 && display.value == 106);
    assert(native.last_call == 6 && native.args[0] == 8 && native.args[1] == reinterpret_cast<std::uintptr_t>(&display));
    assert(factory->CheckDeviceType(9, DEVICE, FORMAT1, FORMAT2, 1) == 107);
    assert((native.last_call == 7 && native.args == std::array<std::uintptr_t, 6>{9, DEVICE, FORMAT1, FORMAT2, 1, 0}));
    assert(factory->CheckDeviceFormat(10, DEVICE, FORMAT1, 77, RESOURCE, FORMAT2) == 108);
    assert((native.last_call == 8 && native.args == std::array<std::uintptr_t, 6>{10, DEVICE, FORMAT1, 77, RESOURCE, FORMAT2}));
    assert(factory->CheckDeviceMultiSampleType(11, DEVICE, FORMAT2, 1, MULTISAMPLE) == 109);
    assert((native.last_call == 9 && native.args == std::array<std::uintptr_t, 6>{11, DEVICE, FORMAT2, 1, MULTISAMPLE, 0}));
    assert(factory->CheckDepthStencilMatch(12, DEVICE, FORMAT1, FORMAT2, FORMAT3) == 110);
    assert((native.last_call == 10 && native.args == std::array<std::uintptr_t, 6>{12, DEVICE, FORMAT1, FORMAT2, FORMAT3, 0}));
    D3DCAPS8 caps{};
    assert(factory->GetDeviceCaps(13, DEVICE, &caps) == 111 && caps.value == 111);
    assert(native.last_call == 11 && native.args[0] == 13 && native.args[1] == DEVICE &&
           native.args[2] == reinterpret_cast<std::uintptr_t>(&caps));
    assert(factory->GetAdapterMonitor(14) == reinterpret_cast<HMONITOR>(0x1234));
    assert(native.last_call == 12 && native.args[0] == 14);

    D3DPRESENT_PARAMETERS presentation{};
    auto* output = reinterpret_cast<IDirect3DDevice8*>(0x1234);
    assert(factory->CreateDevice(0, DEVICE, nullptr, 0, &presentation, &output) == E_NOTIMPL && !output);
    assert(factory->CreateDevice(0, DEVICE, nullptr, 0, &presentation, nullptr) == E_POINTER);
    assert(native.device_calls == 0);
    assert(factory->Release() == 0 && native.releases == 1);
}
}  // namespace

int main() {
    FactoryComIdentityAndLifetime();
    FactoryMethodsForwardAndCreationFailsClosed();
}
