#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <array>
#include <cstring>
#include <iostream>

using Microsoft::WRL::ComPtr;

bool negotiate(HMODULE module) {
    const auto function = reinterpret_cast<PFN_xrNegotiateLoaderApiLayerInterface>(
        GetProcAddress(module, "xrNegotiateLoaderApiLayerInterface"));
    if (!function) return false;
    XrNegotiateLoaderInfo loader{};
    loader.structType=XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    loader.structVersion=XR_LOADER_INFO_STRUCT_VERSION;
    loader.structSize=sizeof(loader);
    loader.minInterfaceVersion=1; loader.maxInterfaceVersion=XR_CURRENT_LOADER_API_LAYER_VERSION;
    loader.minApiVersion=XR_MAKE_VERSION(1,0,0); loader.maxApiVersion=XR_CURRENT_API_VERSION;
    XrNegotiateApiLayerRequest request{};
    request.structType=XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST;
    request.structVersion=XR_API_LAYER_INFO_STRUCT_VERSION;
    request.structSize=sizeof(request);
    return XR_SUCCEEDED(function(&loader,"XR_APILAYER_XRFrameBridge_diagnostic",&request));
}

int main(int argc,char** argv) {
    if(argc!=3 && argc!=4) return 64;
    const bool off=std::strcmp(argv[2],"off")==0;
    const bool modified=std::strcmp(argv[2],"modified")==0;
    SetEnvironmentVariableW(L"XRFG_LATE60",off?nullptr:L"1");
    if(argc==4) {
        if(!LoadLibraryA(argv[3])) return 5;
        std::cout << "FFR_CHAIN loaded=1" << std::endl;
    }
    HMODULE module=LoadLibraryA(argv[1]);
    if(!module || !negotiate(module)) return 1;
    const auto window=CreateWindowExW(0,L"STATIC",L"private D3D11 fixture",WS_POPUP,
        0,0,16,16,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!window) return 2;
    unsigned tested{}, changed{}, interfaces{};
    const auto check_device=[&](bool swap,D3D_DRIVER_TYPE driver,UINT flags) {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<IDXGISwapChain> chain;
        HRESULT result;
        if(swap) {
            DXGI_SWAP_CHAIN_DESC description{};
            description.BufferDesc.Width=description.BufferDesc.Height=16;
            description.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
            description.SampleDesc.Count=1; description.BufferCount=2;
            description.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
            description.OutputWindow=window; description.Windowed=TRUE;
            result=D3D11CreateDeviceAndSwapChain(nullptr,driver,nullptr,flags,nullptr,0,
                D3D11_SDK_VERSION,&description,&chain,&device,nullptr,&context);
        } else {
            result=D3D11CreateDevice(nullptr,driver,nullptr,flags,nullptr,0,
                D3D11_SDK_VERSION,&device,nullptr,&context);
        }
        if(FAILED(result)) {
            std::cerr << "CREATE_FAILED swap=" << swap << " driver=" << driver
                << " flags=" << flags << " hr=" << result << std::endl;
            return false;
        }
        const UINT expected=modified?flags & ~D3D11_CREATE_DEVICE_SINGLETHREADED:flags;
        if(device->GetCreationFlags()!=expected) {
            std::cerr << "FLAGS_FAILED requested=" << flags << " actual="
                << device->GetCreationFlags() << " expected=" << expected << std::endl;
            return false;
        }
        ComPtr<ID3D11Multithread> protection;
        const bool available=SUCCEEDED(context.As(&protection));
        if(available!=((expected & D3D11_CREATE_DEVICE_SINGLETHREADED)==0)) {
            std::cerr << "INTERFACE_FAILED swap=" << swap << " driver=" << driver
                << " flags=" << flags << " available=" << available << std::endl;
            return false;
        }
        if(available) {
            protection->SetMultithreadProtected(TRUE);
            protection->Enter(); protection->Leave(); ++interfaces;
        }
        changed+=expected!=flags; ++tested;
        return true;
    };
    for(bool swap:{false,true})
        for(auto driver:{D3D_DRIVER_TYPE_HARDWARE,D3D_DRIVER_TYPE_WARP})
            for(UINT flags:{0u,1u,32u,33u})
                if(!check_device(swap,driver,flags)) return 3;
    // Recycle loader references like OpenComposite's bootstrap handoff. An
    // enabled hook retains a single module owner, not one per instance/session.
    for(unsigned cycle=0;cycle<3;++cycle) {
        FreeLibrary(module);
        module=LoadLibraryA(argv[1]);
        if(!module || !negotiate(module) ||
            !check_device(false,D3D_DRIVER_TYPE_HARDWARE,33)) return 4;
    }
    FreeLibrary(module);
    DestroyWindow(window);
    std::cout << "DEVICE_THREADING_PASS mode=" << argv[2] << " devices=" << tested
        << " changed=" << changed << " interfaces=" << interfaces
        << " other_flags_preserved=1 bootstrap_cycles=3\n";
    return 0;
}
