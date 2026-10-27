// A private native client of the same OpenComposite/VDXR/FFR path, not VaM.
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <openvr.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

using Microsoft::WRL::ComPtr;

template<class Function>
Function method(void* instance, unsigned slot) {
    return reinterpret_cast<Function>((*reinterpret_cast<void***>(instance))[slot]);
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) return 64;
    const std::filesystem::path game(argv[1]);
    const std::filesystem::path ffr(argv[2]);
    const unsigned frames = std::stoul(argv[3]);
    if (frames < 10 || frames > 1800) return 64;
    SetDllDirectoryW((game / L"VaM_Data/Plugins").c_str());
    const HMODULE proxy = LoadLibraryW(ffr.c_str());
    if (!proxy) { std::cout << "FFR_LOAD_FAIL " << GetLastError() << std::endl; return 1; }
    std::cout << "VR_STAGE FFR_loaded" << std::endl;
    const HMODULE api = LoadLibraryW((game / L"VaM_Data/Plugins/openvr_api.dll").c_str());
    if (!api) { std::cout << "OPENVR_LOAD_FAIL " << GetLastError() << std::endl; return 2; }
    std::cout << "VR_STAGE OpenComposite_loaded" << std::endl;
    const auto initialize = reinterpret_cast<std::uint32_t(VR_CALLTYPE*)(vr::EVRInitError*,vr::EVRApplicationType,const char*)>(GetProcAddress(api,"VR_InitInternal2"));
    const auto get = reinterpret_cast<void*(VR_CALLTYPE*)(const char*,vr::EVRInitError*)>(GetProcAddress(api,"VR_GetGenericInterface"));
    const auto shutdown = reinterpret_cast<void(VR_CALLTYPE*)()>(GetProcAddress(api,"VR_ShutdownInternal"));
    if (!initialize || !get || !shutdown) return 3;
    vr::EVRInitError error{};
    initialize(&error, vr::VRApplication_Scene, nullptr);
    std::cout << "VR_STAGE initialized error=" << error << std::endl;
    if (error != vr::VRInitError_None) {
        std::cout << "VR_INIT_FAIL error=" << error << std::endl; return 4;
    }
    void* system = get("IVRSystem_015", &error);
    void* compositor = get("IVRCompositor_020", &error);
    if (!system || !compositor || error != vr::VRInitError_None) return 5;
    std::cout << "VR_STAGE interfaces_ready" << std::endl;
    using Size = void(*)(void*,std::uint32_t*,std::uint32_t*);
    using Wait = vr::EVRCompositorError(*)(void*,vr::TrackedDevicePose_t*,std::uint32_t,vr::TrackedDevicePose_t*,std::uint32_t);
    using Submit = vr::EVRCompositorError(*)(void*,vr::EVREye,const vr::Texture_t*,const vr::VRTextureBounds_t*,vr::EVRSubmitFlags);
    using Handoff = void(*)(void*);
    std::uint32_t width{},height{};
    method<Size>(system,0)(system,&width,&height);
    width = (std::min)(width,1536u);
    height = (std::min)(height,1536u);
    if (!width || !height) return 6;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediate, deferred;
    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,2,
            D3D11_SDK_VERSION,&device,&level,&immediate)) ||
        FAILED(device->CreateDeferredContext(0,&deferred))) return 7;
    ComPtr<ID3D11Multithread> multithread;
    if (FAILED(immediate.As(&multithread))) return 8;
    multithread->SetMultithreadProtected(TRUE);
    std::cout << "VR_STAGE D3D11_ready" << std::endl;
    D3D11_TEXTURE2D_DESC description{};
    description.Width=width; description.Height=height;
    description.MipLevels=description.ArraySize=description.SampleDesc.Count=1;
    description.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    struct Eye { ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11RenderTargetView> target; };
    std::array<std::array<Eye,2>,3> ring;
    for(auto& pair:ring) for(auto& eye:pair)
        if(FAILED(device->CreateTexture2D(&description,nullptr,&eye.texture)) ||
           FAILED(device->CreateRenderTargetView(eye.texture.Get(),nullptr,&eye.target))) return 9;
    D3D11_SAMPLER_DESC sampler_info{};
    sampler_info.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_info.AddressU=sampler_info.AddressV=sampler_info.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_info.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    if(FAILED(device->CreateSamplerState(&sampler_info,&sampler))) return 10;
    std::array<vr::TrackedDevicePose_t,vr::k_unMaxTrackedDeviceCount> poses{};
    const auto start=std::chrono::steady_clock::now();
    for(unsigned frame=0;frame<frames;++frame) {
        const auto entered=std::chrono::steady_clock::now();
        const auto waited=method<Wait>(compositor,2)(compositor,poses.data(),static_cast<std::uint32_t>(poses.size()),nullptr,0);
        if(waited!=vr::VRCompositorError_None) {std::cout<<"WAIT_FAIL error="<<waited<<std::endl;return 11;}
        auto& pair=ring[frame%ring.size()];
        ID3D11SamplerState* state=sampler.Get();
        deferred->PSSetSamplers(0,1,&state);
        const D3D11_VIEWPORT viewport{0,0,static_cast<float>(width),static_cast<float>(height),0,1};
        deferred->RSSetViewports(1,&viewport);
        for(unsigned eye=0;eye<2;++eye) {
            ID3D11RenderTargetView* target=pair[eye].target.Get();
            deferred->OMSetRenderTargets(1,&target,nullptr);
            const float color[]{0.03F+0.01F*static_cast<float>(frame%5),0.03F,0.03F,1};
            for(unsigned work=0;work<32;++work) deferred->ClearRenderTargetView(target,color);
        }
        ComPtr<ID3D11CommandList> commands;
        if(FAILED(deferred->FinishCommandList(FALSE,&commands))) return 12;
        immediate->ExecuteCommandList(commands.Get(),FALSE);
        for(unsigned eye=0;eye<2;++eye) {
            const vr::Texture_t texture{pair[eye].texture.Get(),vr::TextureType_DirectX,vr::ColorSpace_Auto};
            const auto submitted=method<Submit>(compositor,5)(compositor,static_cast<vr::EVREye>(eye),&texture,nullptr,vr::Submit_Default);
            if(submitted!=vr::VRCompositorError_None) {std::cout<<"SUBMIT_FAIL error="<<submitted<<std::endl;return 13;}
        }
        method<Handoff>(compositor,7)(compositor);
        if(frame%30==0) std::cout<<"VR_PROGRESS real_pairs="<<frame+1<<std::endl;
        const auto duration=(frame==60 || frame==100)?std::chrono::milliseconds(62):std::chrono::milliseconds(33);
        std::this_thread::sleep_until(entered+duration);
    }
    shutdown();
    const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::cout<<"VR_SMOKE_PASS real_pairs="<<frames<<" elapsed_s="<<elapsed
        <<" deferred_commands=1 FFR_loaded=1 actual_runtime=1 shutdown=1"<<std::endl;
    // The OS releases this diagnostic client's device and loaded native stack.
    return 0;
}
