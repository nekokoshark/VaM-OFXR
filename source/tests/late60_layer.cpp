
// Reuses the package fake runtime, but asserts the new output/cadence contract.
#define main upstream_layer_test_main
#include "layer_call_chain.cpp"
#undef main
#include <unordered_map>

std::mutex timing_mutex;
std::vector<std::chrono::steady_clock::time_point> submission_times;
bool measuring = false;
std::atomic_bool application_context_held{false};
std::atomic_uint32_t runtime_context_overlaps{0};
bool dynamic_mode=false, timeout_mode=false, budget_mode=false, split_mode=false;
std::mutex ownership_mutex;
std::unordered_map<XrSwapchain,int> image_phase;
unsigned ownership_errors=0;
std::atomic_uint32_t nonblocking_waits{0}, injected_timeouts{0}, injected_releases{0};
std::atomic_bool expect_real_after_release_fault{false};
std::atomic_uint32_t fault_fallback_errors{0};
XRAPI_ATTR XrResult XRAPI_CALL checked_acquire(XrSwapchain s,
    const XrSwapchainImageAcquireInfo* info,std::uint32_t* index) {
    const XrResult r=fake_acquire_swapchain_image(s,info,index);
    if(XR_SUCCEEDED(r)) { std::scoped_lock lock(ownership_mutex);
        if(image_phase[s]!=0) ++ownership_errors; image_phase[s]=1; }
    return r;
}
XRAPI_ATTR XrResult XRAPI_CALL checked_wait(XrSwapchain s,const XrSwapchainImageWaitInfo* info) {
    { std::scoped_lock lock(ownership_mutex); if(image_phase[s]!=1) ++ownership_errors; }
    if(info && info->timeout==0) {
        const auto n=++nonblocking_waits;
        if(timeout_mode && n%5==1) { ++injected_timeouts; return XR_TIMEOUT_EXPIRED; }
        if(budget_mode) std::this_thread::sleep_for(std::chrono::milliseconds(12));
        if(timeout_mode && n==3) { ++injected_releases; g_fail_next_synthetic_release.store(true); }
    }
    const XrResult r=fake_wait_swapchain_image(s,info);
    if(r==XR_SUCCESS || r==XR_SESSION_LOSS_PENDING) {
        std::scoped_lock lock(ownership_mutex); image_phase[s]=2;
    }
    return r;
}
XRAPI_ATTR XrResult XRAPI_CALL checked_release(XrSwapchain s,const XrSwapchainImageReleaseInfo* info) {
    { std::scoped_lock lock(ownership_mutex); if(image_phase[s]!=2) ++ownership_errors; }
    if(split_mode && s==g_synthetic_swapchain_right && nonblocking_waits.load()>8 &&
        injected_releases.exchange(1)==0) {
        expect_real_after_release_fault.store(true); return XR_ERROR_RUNTIME_FAILURE;
    }
    const XrResult r=fake_release_swapchain_image(s,info);
    if(XR_FAILED(r) && is_synthetic_swapchain(s)) expect_real_after_release_fault.store(true);
    if(XR_SUCCEEDED(r)) { std::scoped_lock lock(ownership_mutex); image_phase[s]=0; }
    return r;
}
void precise_sleep(unsigned milliseconds) {
    HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,0x2,TIMER_ALL_ACCESS);
    if(!timer) throw std::runtime_error("high resolution test timer");
    LARGE_INTEGER due{}; due.QuadPart=-static_cast<LONGLONG>(milliseconds)*10'000;
    const bool ok=SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE) &&
        WaitForSingleObject(timer,2000)==WAIT_OBJECT_0;
    CloseHandle(timer);
    if(!ok) throw std::runtime_error("test timer wait");
}
bool initialize_split_hardware() {
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    if(FAILED(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)))) return false;
    for(UINT index=0;;++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if(factory->EnumAdapters1(index,&adapter)==DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        if(FAILED(adapter->GetDesc1(&desc)) || desc.VendorId!=0x10de) continue;
        if(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&g_device)))) break;
    }
    if(!g_device) return false;
    D3D12_COMMAND_QUEUE_DESC queue{}; queue.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    if(FAILED(g_device->CreateCommandQueue(&queue,IID_PPV_ARGS(&g_queue)))) return false;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width=texture.Height=4; texture.DepthOrArraySize=texture.MipLevels=1;
    texture.Format=DXGI_FORMAT_R8G8B8A8_UNORM; texture.SampleDesc.Count=1;
    texture.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for(auto* images:{&g_application_swapchain_images,&g_current_swapchain_images,
        &g_synthetic_swapchain_images,&g_second_synthetic_swapchain_images,
        &g_application_swapchain_right_images,&g_current_swapchain_right_images,&g_synthetic_swapchain_right_images})
        for(auto& image:*images) if(FAILED(g_device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,
            &texture,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&image)))) return false;
    return true;
}
XRAPI_ATTR XrResult XRAPI_CALL timed_end(XrSession s, const XrFrameEndInfo* info) {
    if (application_context_held.load() && GetCurrentThreadId() != g_test_application_thread_id)
        runtime_context_overlaps.fetch_add(1);
    const XrResult result = fake_end_frame(s,info);
    if(expect_real_after_release_fault.exchange(false) && XR_SUCCEEDED(result)) {
        std::scoped_lock lock(g_frame_loop_mutex);
        if(g_end_records.empty() || (g_end_records.back().target!=SubmittedTarget::current &&
            g_end_records.back().target!=SubmittedTarget::original)) ++fault_fallback_errors;
    }
    std::scoped_lock lock(timing_mutex);
    if (measuring && XR_SUCCEEDED(result)) submission_times.push_back(std::chrono::steady_clock::now());
    return result;
}
XRAPI_ATTR XrResult XRAPI_CALL clocked_wait_frame(
    XrSession,
    const XrFrameWaitInfo*,
    XrFrameState* frame_state) {
    if (frame_state == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    const std::uint32_t wait_call =
        g_wait_frame_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g_steamvr_presenter_mode) {
        if (GetCurrentThreadId() == g_test_application_thread_id &&
            wait_call >= 3 && (wait_call % 2) == 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(7));
        } else if (GetCurrentThreadId() != g_test_application_thread_id) {
            // No artificial Sleep(1) in the cadence fixture: on this host it rounds to 15.6ms.
        }
    }
    if (g_flight_simulator_mode &&
        GetCurrentThreadId() != g_test_application_thread_id) {
        // No artificial Sleep(1) in the cadence fixture: on this host it rounds to 15.6ms.
    }
    if (g_wait_begin_handoff_mode.load(std::memory_order_acquire) &&
        g_wait_begin_handoff_calls.fetch_add(1, std::memory_order_acq_rel) == 1) {
        g_second_handoff_wait_entered.store(true, std::memory_order_release);
        const auto escape_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (!g_allow_second_handoff_wait_return.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < escape_deadline) {
            std::this_thread::yield();
        }
    }
    if (g_fail_next_wait_frame.exchange(false, std::memory_order_acq_rel)) {
        if (!g_log_path.empty()) {
            std::ofstream stream(g_log_path, std::ios::out | std::ios::app);
            stream << "[XRFG-FAKE] downstream wait frame failed\n";
        }
        return XR_ERROR_RUNTIME_FAILURE;
    }
    frame_state->predictedDisplayTime = g_next_display_time;
    frame_state->predictedDisplayPeriod = kFakeDisplayPeriod;
    frame_state->shouldRender =
        g_next_wait_should_not_render.exchange(false, std::memory_order_acq_rel)
            ? XR_FALSE
            : XR_TRUE;
    g_next_display_time += g_flight_simulator_mode
        ? kFakeDisplayPeriod * 3
        : 100;
    {
        std::scoped_lock lock(g_frame_loop_mutex);
        g_waited_display_times.push_back(frame_state->predictedDisplayTime);
    }
    if (!g_log_path.empty()) {
        std::ofstream stream(g_log_path, std::ios::out | std::ios::app);
        stream << "[XRFG-FAKE] downstream wait frame time="
               << frame_state->predictedDisplayTime << '\n';
    }
    return XR_SUCCESS;
}


XRAPI_ATTR XrResult XRAPI_CALL timed_get_proc(XrInstance i,const char* name,PFN_xrVoidFunction* out) {
    if (std::strcmp(name,"xrWaitFrame")==0) { *out=reinterpret_cast<PFN_xrVoidFunction>(clocked_wait_frame); return XR_SUCCESS; }
    if (std::strcmp(name,"xrEndFrame")==0) { *out=reinterpret_cast<PFN_xrVoidFunction>(timed_end); return XR_SUCCESS; }
    if(dynamic_mode && std::strcmp(name,"xrAcquireSwapchainImage")==0) { *out=reinterpret_cast<PFN_xrVoidFunction>(checked_acquire); return XR_SUCCESS; }
    if(dynamic_mode && std::strcmp(name,"xrWaitSwapchainImage")==0) { *out=reinterpret_cast<PFN_xrVoidFunction>(checked_wait); return XR_SUCCESS; }
    if(dynamic_mode && std::strcmp(name,"xrReleaseSwapchainImage")==0) { *out=reinterpret_cast<PFN_xrVoidFunction>(checked_release); return XR_SUCCESS; }
    return fake_get_instance_proc_addr(i,name,out);
}
int main(int argc,char** argv) {
    if(argc!=4) return 64;
    const bool baseline=std::strcmp(argv[3],"baseline")==0;
    const bool contention_baseline=std::strcmp(argv[3],"contended-baseline")==0;
    const bool contention_modified=std::strcmp(argv[3],"contended-modified")==0;
    const bool single_threaded=std::strcmp(argv[3],"singlethreaded")==0;
    dynamic_mode=std::strncmp(argv[3],"dynamic",7)==0;
    timeout_mode=std::strcmp(argv[3],"dynamic-timeout")==0;
    budget_mode=std::strcmp(argv[3],"dynamic-budget")==0;
    split_mode=std::strcmp(argv[3],"dynamic-split")==0;
    const bool dynamic_baseline=std::strcmp(argv[3],"dynamic-baseline")==0;
    if(single_threaded) g_d3d11_creation_flags |= D3D11_CREATE_DEVICE_SINGLETHREADED;
    SetEnvironmentVariableW(L"XRFG_LATE60",L"1");
    g_test_application_thread_id=GetCurrentThreadId();
    g_d3d11_interop_mode=!split_mode;
    g_split_eye_mode=split_mode;
    g_steamvr_presenter_mode=true;
    g_steamvr_runtime_mode=true;
    g_destroy_pending_swapchain=true;
    g_destroy_pending_space=true;
    g_log_path=argv[2];
    std::ofstream reset(g_log_path,std::ios::trunc); reset.close();
    if(!single_threaded && !(split_mode ? initialize_split_hardware() : initialize_d3d11())) return 1;
    const HMODULE module = LoadLibraryA(argv[1]);
    if (module == nullptr) {
        std::cerr << "LoadLibrary failed: " << GetLastError() << '\n';
        return EXIT_FAILURE;
    }

    const auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderApiLayerInterface>(
        GetProcAddress(module, "xrNegotiateLoaderApiLayerInterface"));
    if (negotiate == nullptr) {
        FreeLibrary(module);
        return EXIT_FAILURE;
    }

    XrNegotiateLoaderInfo loader_info{};
    loader_info.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    loader_info.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    loader_info.structSize = sizeof(loader_info);
    loader_info.minInterfaceVersion = 1;
    loader_info.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    loader_info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    loader_info.maxApiVersion = XR_CURRENT_API_VERSION;

    XrNegotiateApiLayerRequest request{};
    request.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST;
    request.structVersion = XR_API_LAYER_INFO_STRUCT_VERSION;
    request.structSize = sizeof(request);
    if (XR_FAILED(negotiate(&loader_info, kLayerName, &request))) {
        FreeLibrary(module);
        return EXIT_FAILURE;
    }

    if(single_threaded) {
        if(!initialize_d3d11()) return 1;
        Microsoft::WRL::ComPtr<ID3D11Multithread> protection;
        const HRESULT hr=g_d3d11_context.As(&protection);
        std::cout << "DEVICE_INPUT requested_flags=" << g_d3d11_creation_flags
            << " actual_flags=" << g_d3d11_device->GetCreationFlags()
            << " multithread_hr=" << hr << std::endl;
    }
    XrApiLayerNextInfo next_info{};
    next_info.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO;
    next_info.structVersion = XR_API_LAYER_NEXT_INFO_STRUCT_VERSION;
    next_info.structSize = sizeof(next_info);
    strcpy_s(next_info.layerName, kLayerName);
    next_info.nextGetInstanceProcAddr = timed_get_proc;
    next_info.nextCreateApiLayerInstance = fake_create_api_layer_instance;

    XrApiLayerCreateInfo layer_info{};
    layer_info.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO;
    layer_info.structVersion = XR_API_LAYER_CREATE_INFO_STRUCT_VERSION;
    layer_info.structSize = sizeof(layer_info);
    layer_info.nextInfo = &next_info;

    XrInstanceCreateInfo instance_info{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(instance_info.applicationInfo.applicationName, "XRFG fake runtime test");
    strcpy_s(instance_info.applicationInfo.engineName, "XRFG tests");
    instance_info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);

    XrInstance instance = XR_NULL_HANDLE;
    if (XR_FAILED(request.createApiLayerInstance(&instance_info, &layer_info, &instance)) ||
        instance != g_instance) {
        FreeLibrary(module);
        return EXIT_FAILURE;
    }

    const auto create_session = get_layer_function<PFN_xrCreateSession>(request.getInstanceProcAddr, "xrCreateSession");
    const auto destroy_session = get_layer_function<PFN_xrDestroySession>(request.getInstanceProcAddr, "xrDestroySession");
    const auto begin_session = get_layer_function<PFN_xrBeginSession>(request.getInstanceProcAddr, "xrBeginSession");
    const auto end_session = get_layer_function<PFN_xrEndSession>(request.getInstanceProcAddr, "xrEndSession");
    const auto wait_frame = get_layer_function<PFN_xrWaitFrame>(request.getInstanceProcAddr, "xrWaitFrame");
    const auto begin_frame = get_layer_function<PFN_xrBeginFrame>(request.getInstanceProcAddr, "xrBeginFrame");
    const auto end_frame = get_layer_function<PFN_xrEndFrame>(request.getInstanceProcAddr, "xrEndFrame");
    const auto locate_views = get_layer_function<PFN_xrLocateViews>(request.getInstanceProcAddr, "xrLocateViews");
    const auto create_swapchain = get_layer_function<PFN_xrCreateSwapchain>(request.getInstanceProcAddr, "xrCreateSwapchain");
    const auto destroy_swapchain = get_layer_function<PFN_xrDestroySwapchain>(request.getInstanceProcAddr, "xrDestroySwapchain");
    const auto destroy_space = get_layer_function<PFN_xrDestroySpace>(request.getInstanceProcAddr, "xrDestroySpace");
    const auto enumerate_images = get_layer_function<PFN_xrEnumerateSwapchainImages>(request.getInstanceProcAddr, "xrEnumerateSwapchainImages");
    const auto acquire_image = get_layer_function<PFN_xrAcquireSwapchainImage>(request.getInstanceProcAddr, "xrAcquireSwapchainImage");
    const auto wait_image = get_layer_function<PFN_xrWaitSwapchainImage>(request.getInstanceProcAddr, "xrWaitSwapchainImage");
    const auto release_image = get_layer_function<PFN_xrReleaseSwapchainImage>(request.getInstanceProcAddr, "xrReleaseSwapchainImage");
    const auto destroy_instance = get_layer_function<PFN_xrDestroyInstance>(request.getInstanceProcAddr, "xrDestroyInstance");

    if (!create_session || !destroy_session || !begin_session || !end_session || !wait_frame ||
        !begin_frame || !end_frame || !locate_views || !create_swapchain || !destroy_swapchain ||
        !enumerate_images || !acquire_image || !wait_image || !release_image || !destroy_instance) {
        FreeLibrary(module);
        return EXIT_FAILURE;
    }

    XrGraphicsBindingD3D12KHR graphics_binding{
        XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    graphics_binding.device = g_device.Get();
    graphics_binding.queue = g_queue.Get();
    XrGraphicsBindingD3D11KHR d3d11_graphics_binding{
        XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    d3d11_graphics_binding.device = g_d3d11_device.Get();
    XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
    session_info.next = g_d3d11_interop_mode
        ? static_cast<const void*>(&d3d11_graphics_binding)
        : static_cast<const void*>(&graphics_binding);
    session_info.systemId = 1;
    XrSession session = XR_NULL_HANDLE;
    XrSessionBeginInfo session_begin_info{XR_TYPE_SESSION_BEGIN_INFO};
    session_begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    if (XR_FAILED(create_session(instance, &session_info, &session)) ||
        XR_FAILED(begin_session(session, &session_begin_info))) {
        FreeLibrary(module);
        return EXIT_FAILURE;
    }

    XrSwapchainCreateInfo swapchain_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchain_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    swapchain_info.format = static_cast<std::int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM);
    swapchain_info.sampleCount = 1;
    swapchain_info.width = g_double_wide_mode ? 8 : 4;
    swapchain_info.height = 4;
    swapchain_info.faceCount = 1;
    swapchain_info.arraySize =
        (g_split_eye_mode || g_double_wide_mode) ? 1 : 2;
    swapchain_info.mipCount = g_d3d11_interop_mode ? 3 : 1;


    XrSwapchain swapchain{};
    if (XR_FAILED(create_swapchain(session,&swapchain_info,&swapchain))) return 2;
    XrSwapchain right{};
    std::array<XrSwapchainImageD3D11KHR,3> images{};
    for(auto& image:images) image.type=XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    std::uint32_t count{};
    if(split_mode) {
        std::array<XrSwapchainImageD3D12KHR,3> d12{};
        for(auto& image:d12) image.type=XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
        if(XR_FAILED(enumerate_images(swapchain,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(d12.data()))) || count!=3) return 3;
        // The package fake runtime assigns each application's private current
        // and synthetic handles during enumeration, before the next eye.
        if(XR_FAILED(create_swapchain(session,&swapchain_info,&right))) return 2;
        if(XR_FAILED(enumerate_images(right,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(d12.data()))) || count!=3) return 3;
    } else if(XR_FAILED(enumerate_images(swapchain,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()))) || count!=3) return 3;
    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo image_wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; image_wait.timeout=XR_INFINITE_DURATION;
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    std::array<XrCompositionLayerProjectionView,2> views{};
    for(unsigned eye=0;eye<2;++eye) {
        views[eye].type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        views[eye].pose.orientation.w=1;
        views[eye].fov={-0.8F,0.8F,0.7F,-0.7F};
        views[eye].subImage.swapchain=split_mode && eye==1 ? right : swapchain;
        views[eye].subImage.imageRect.extent={4,4};
        views[eye].subImage.imageArrayIndex=split_mode ? 0 : eye;
    }
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.space=g_space; projection.viewCount=2; projection.views=views.data();
    const XrCompositionLayerBaseHeader* layers[]={reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection)};
    bool virtual_period_seen=false;
    auto next_frame = [&]() {
        XrFrameWaitInfo w{XR_TYPE_FRAME_WAIT_INFO}; XrFrameState f{XR_TYPE_FRAME_STATE};
        XrFrameBeginInfo b{XR_TYPE_FRAME_BEGIN_INFO};
        if(XR_FAILED(wait_frame(session,&w,&f)) || XR_FAILED(begin_frame(session,&b))) return false;
        virtual_period_seen |= f.predictedDisplayPeriod == (baseline?20'000'000:33'333'334);
        std::uint32_t index{};
        if(XR_FAILED(acquire_image(swapchain,&acquire_info,&index)) || XR_FAILED(wait_image(swapchain,&image_wait)) ||
            XR_FAILED(release_image(swapchain,&release_info))) return false;
        if(split_mode && (XR_FAILED(acquire_image(right,&acquire_info,&index)) || XR_FAILED(wait_image(right,&image_wait)) ||
            XR_FAILED(release_image(right,&release_info)))) return false;
        XrFrameEndInfo e{XR_TYPE_FRAME_END_INFO}; e.displayTime=f.predictedDisplayTime;
        e.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE; e.layerCount=1; e.layers=layers;
        return XR_SUCCEEDED(end_frame(session,&e));
    };
    // Establish real pairs with ~30 FPS input, then insert transient late frames.
    const unsigned frames=dynamic_mode ? 44 : 18;
    for(unsigned frame=0;frame<frames;++frame) {
        if(!next_frame()) {
            std::cerr<<"frame failure "<<frame<<'\n';
            // An expected failing binary must still join its presenter before
            // the private test process tears down global session state.
            end_session(session);
            destroy_swapchain(swapchain);
            destroy_session(session);
            destroy_instance(instance);
            FreeLibrary(module);
            return 4;
        }
        if(frame==3) { std::scoped_lock lock(timing_mutex); measuring=true; }
        if(dynamic_mode) {
            constexpr unsigned waits[]={33,43,42,62,59,100};
            precise_sleep(waits[std::min(frame/8,5U)]);
        } else std::this_thread::sleep_for(std::chrono::milliseconds(frame==7 || frame==12 ? 62 : 33));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(55));
    const auto covers=int(g_synthetic_acquire_calls.load())-int(frames-1)*(split_mode ? 2 : 1);
    { std::scoped_lock lock(timing_mutex); measuring=false; }
    if (contention_baseline || contention_modified || single_threaded) {
        Microsoft::WRL::ComPtr<ID3D11Multithread> context_guard;
        if (FAILED(g_d3d11_context.As(&context_guard))) return 10;
        context_guard->SetMultithreadProtected(TRUE);
        context_guard->Enter();
        application_context_held.store(true);
        // A real D3D11 context lock represents Unity's grouped render work.
        // Runtime callbacks deliberately remain nonblocking, so the faulty
        // build is observable without hanging the test process or the GPU.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        application_context_held.store(false);
        context_guard->Leave();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if(XR_FAILED(destroy_space(g_space))) return 5;
    std::this_thread::sleep_for(std::chrono::milliseconds(45));
    if(XR_FAILED(destroy_swapchain(swapchain))) return 6;
    if(split_mode && XR_FAILED(destroy_swapchain(right))) return 6;
    std::this_thread::sleep_for(std::chrono::milliseconds(45));
    if(XR_FAILED(end_session(session)) || XR_FAILED(destroy_session(session)) || XR_FAILED(destroy_instance(instance))) return 7;
    FreeLibrary(module);
    std::vector<double> gaps;
    {
        std::scoped_lock lock(timing_mutex);
        for(std::size_t i=1;i<submission_times.size();++i)
            gaps.push_back(std::chrono::duration<double,std::milli>(submission_times[i]-submission_times[i-1]).count());
    }
    if(gaps.empty()) return 8;
    double sum=0;
    for(const auto gap:gaps) sum+=gap;
    const auto mean=sum/gaps.size();
    std::cout<<"cadence samples="<<gaps.size()<<" gaps_ms=";
    for(const auto gap:gaps) std::cout<<gap<<',';
    std::cout<<'\n';
    std::sort(gaps.begin(),gaps.end());
    const auto median=gaps[gaps.size()/2];
    bool stereo=true;
    for(const auto& record:g_end_records) if(record.target==SubmittedTarget::mixed) stereo=false;
    const auto overlaps=runtime_context_overlaps.load();
    const bool context_ok=(!contention_baseline || overlaps>0) && (!(contention_modified || single_threaded) || overlaps==0);
    bool ownership_ok=true;
    { std::scoped_lock lock(ownership_mutex);
        for(const auto& [s,phase]:image_phase) { static_cast<void>(s); if(phase!=0) ownership_ok=false; }
        ownership_ok &= ownership_errors==0; }
    const bool ok=virtual_period_seen && stereo && g_submission_after_destroy.load()==0 && context_ok && ownership_ok && fault_fallback_errors.load()==0 &&
        (baseline ? covers==0 : covers>=2 && mean>=(dynamic_mode ? 15.0 : 15.6) && mean<(dynamic_mode ? 21.0 : 17.9)) &&
        (!dynamic_mode || dynamic_baseline || nonblocking_waits.load()>3) &&
        (!timeout_mode || (injected_timeouts.load()>0 && injected_releases.load()>0)) &&
        (!split_mode || injected_releases.load()>0);
    std::cout<<(baseline?"BASELINE_LAYER":"LATE60_LAYER")<<" virtual_period="<<virtual_period_seen
        <<" median_output_ms="<<median<<" mean_output_ms="<<mean<<" extra_covers="<<covers<<" stereo="<<stereo
        <<" after_destroy="<<g_submission_after_destroy.load()<<" result="<<(ok?"PASS":"FAIL")<<'\n';
    if (contention_baseline || contention_modified || single_threaded)
        std::cout << "D3D11_SERIALIZATION runtime_context_overlaps=" << overlaps
            << " expected=" << (contention_baseline ? "nonzero" : "zero")
            << " result=" << (context_ok ? "PASS" : "FAIL") << '\n';
    if(dynamic_mode) std::cout<<"DYNAMIC_LAYER mode="<<argv[3]<<" nonblocking_waits="<<nonblocking_waits.load()
        <<" timeouts="<<injected_timeouts.load()<<" release_fault="<<injected_releases.load()
        <<" ownership_errors="<<ownership_errors<<" ownership_drained="<<ownership_ok
        <<" fault_fallback_errors="<<fault_fallback_errors.load()<<'\n';
    return ok?0:9;
}
