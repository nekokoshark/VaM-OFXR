#include "xrfg/openxr_fps_overlay.hpp"
#include <windows.h>
#include <wrl/client.h>
#include <atomic>
#include <iostream>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace {
xrfg::OpenXrFpsOverlay* active{};
bool fail{};
XrResult XRAPI_CALL downstream(XrSession, const XrFrameEndInfo*) {
    // A runtime callback must not run under the overlay's private mutex.
    static_cast<void>(active->metrics());
    return fail ? XR_ERROR_RUNTIME_FAILURE : XR_SUCCESS;
}
}

int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Multithread> lock;
    if (FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
            D3D11_SDK_VERSION,&device,nullptr,&context)) || FAILED(context.As(&lock))) return 1;
    xrfg::OpenXrFpsOverlay overlay(nullptr,nullptr,0,nullptr,downstream,
        nullptr,nullptr,device.Get(),{});
    active=&overlay;
    XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO};
    lock->SetMultithreadProtected(TRUE);
    lock->Enter();
    fail=true;
    const auto failed=overlay.end_frame(&frame,false,true);
    fail=false;
    const auto passed=overlay.end_frame(&frame,false,true);
    lock->Leave();
    if(failed!=XR_ERROR_RUNTIME_FAILURE || passed!=XR_SUCCESS) return 2;
    std::atomic_bool acquired{false};
    std::thread other([&]{lock->Enter(); acquired.store(true); lock->Leave();});
    other.join();
    if(!acquired.load()) return 3;
    std::cout << "SUBMIT_CONTRACT_PASS callback_metrics=1 recursive_context=1 failure_unlock=1 overlay_optional=1\n";
    return 0;
}
