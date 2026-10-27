#include "xrfg/d3d11_device_threading.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include <windows.h>
#include <d3d11_4.h>
#include <MinHook.h>
#include <mutex>
#include <wrl/client.h>

namespace xrfg {
namespace {
decltype(&D3D11CreateDevice) create_device{};
decltype(&D3D11CreateDeviceAndSwapChain) create_swapchain{};
#if XRFG_SERIALIZE_NVAPI_VRS
using NvapiStateCall = int(__cdecl*)(IUnknown*, void*);
NvapiStateCall set_shading_resource{};
NvapiStateCall set_shading_rates{};

int serialized_nvapi(NvapiStateCall original, IUnknown* context, void* state) {
    Microsoft::WRL::ComPtr<ID3D11Multithread> protection;
    if (context) context->QueryInterface(IID_PPV_ARGS(&protection));
    // These NVAPI extension entry points bypass the immediate context's
    // automatic multithread lock. Use the same recursive lock as xrEndFrame
    // and D3D11, not an independent mutex that the application's draws ignore.
    struct Guard {
        ID3D11Multithread* lock;
        explicit Guard(ID3D11Multithread* value) : lock(value) { if (lock) lock->Enter(); }
        ~Guard() { if (lock) lock->Leave(); }
    } guard(protection.Get());
    return original(context, state);
}

int __cdecl threaded_shading_resource(IUnknown* context, void* state) {
    return serialized_nvapi(set_shading_resource, context, state);
}

int __cdecl threaded_shading_rates(IUnknown* context, void* state) {
    return serialized_nvapi(set_shading_rates, context, state);
}

#endif

HRESULT WINAPI threaded_device(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver,
    HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT count,
    UINT sdk, ID3D11Device** device, D3D_FEATURE_LEVEL* level,
    ID3D11DeviceContext** context) {
    const UINT threaded = flags & ~D3D11_CREATE_DEVICE_SINGLETHREADED;
    const auto result = create_device(adapter, driver, software, threaded,
        levels, count, sdk, device, level, context);
    bridge_flight_logger().event(BridgeFlightOperation::d3d11_threading,
        result, flags, threaded, 1);
    return result;
}

HRESULT WINAPI threaded_swapchain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver,
    HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT count,
    UINT sdk, const DXGI_SWAP_CHAIN_DESC* description, IDXGISwapChain** chain,
    ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context) {
    const UINT threaded = flags & ~D3D11_CREATE_DEVICE_SINGLETHREADED;
    const auto result = create_swapchain(adapter, driver, software, threaded,
        levels, count, sdk, description, chain, device, level, context);
    bridge_flight_logger().event(BridgeFlightOperation::d3d11_threading,
        result, flags, threaded, 2);
    return result;
}
}

bool enable_d3d11_device_threading() noexcept {
    static std::mutex mutex;
    static bool enabled{};
    std::scoped_lock lock(mutex);
    if (enabled) return true;
    const auto module = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return false;
    void* targets[]{reinterpret_cast<void*>(GetProcAddress(module, "D3D11CreateDevice")),
        reinterpret_cast<void*>(GetProcAddress(module, "D3D11CreateDeviceAndSwapChain")), nullptr, nullptr};
    // D3D11 is already used by the layer. Balance this lookup reference.
    FreeLibrary(module);
    if (!targets[0] || !targets[1]) return false;
    HMODULE nvapi{};
#if XRFG_SERIALIZE_NVAPI_VRS
    nvapi = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!nvapi) return false;
    const auto query = reinterpret_cast<void*(__cdecl*)(unsigned)>(
        GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (query) {
        targets[2] = query(0x1b0c2f83); // NvAPI_D3D11_RSSetShadingRateResourceView
        targets[3] = query(0x34f7938f); // NvAPI_D3D11_RSSetViewportsPixelShadingRates
    }
    if (!targets[2] || !targets[3]) { if (nvapi) FreeLibrary(nvapi); return false; }
    constexpr unsigned hook_count = 4;
#else
    constexpr unsigned hook_count = 2;
#endif
    auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        if (nvapi) FreeLibrary(nvapi);
        return false;
    }
    bool created[4]{};
    const auto cleanup = [&] {
        for (unsigned i = 0; i < hook_count; ++i)
            if (created[i]) MH_RemoveHook(targets[i]);
        if (nvapi) FreeLibrary(nvapi);
    };
    status = MH_CreateHook(targets[0], reinterpret_cast<void*>(threaded_device),
        reinterpret_cast<void**>(&create_device));
    if (status == MH_OK) {
        created[0] = true;
        status = MH_CreateHook(targets[1], reinterpret_cast<void*>(threaded_swapchain),
            reinterpret_cast<void**>(&create_swapchain));
        created[1] = status == MH_OK;
    }
#if XRFG_SERIALIZE_NVAPI_VRS
    if (status == MH_OK) {
        status = MH_CreateHook(targets[2], reinterpret_cast<void*>(threaded_shading_resource),
            reinterpret_cast<void**>(&set_shading_resource));
        created[2] = status == MH_OK;
    }
    if (status == MH_OK) {
        status = MH_CreateHook(targets[3], reinterpret_cast<void*>(threaded_shading_rates),
            reinterpret_cast<void**>(&set_shading_rates));
        created[3] = status == MH_OK;
    }
#endif
    if (status == MH_OK) {
        HMODULE owner{};
        // OpenComposite recycles its bootstrap instance before the real Unity
        // device appears. Keep exactly one callback owner for this process;
        // otherwise that legitimate early unload leaves dangling detours.
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(threaded_device), &owner)) {
            cleanup();
            return false;
        }
        // NVAPI extension functions may live in the lazily loaded implementation
        // module. Preserve their trampoline owners across bootstrap unloads too.
        for (unsigned i = 2; i < hook_count; ++i) {
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(targets[i]), &owner)) {
                cleanup();
                return false;
            }
        }
        for (unsigned i = 0; i < hook_count && status == MH_OK; ++i)
            status = MH_QueueEnableHook(targets[i]);
        if (status == MH_OK) status = MH_ApplyQueued();
    }
    bridge_flight_logger().event(BridgeFlightOperation::d3d11_threading,
        status, 0, 0, 0);
    if (status != MH_OK) {
        cleanup();
        return false;
    }
    bridge_flight_logger().event(BridgeFlightOperation::d3d11_threading,
        0, 2, 0, hook_count);
    enabled = true;
    return true;
}
}
