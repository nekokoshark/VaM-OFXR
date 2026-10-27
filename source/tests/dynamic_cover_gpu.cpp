#define main upstream_history_matrix_main
#include "d3d12_history_tests.cpp"
#undef main
#include <chrono>

void dynamic_cover_tests(D3D12WarpFixture& f) {
    constexpr UINT w=1024, h=512;
    std::array<ComPtr<ID3D12Resource>,3> source, current, synthetic;
    std::array<ID3D12Resource*,3> sp{}, cp{}, yp{};
    for (unsigned i=0; i<3; ++i) {
        source[i]=create_source_texture(f,w,h); current[i]=create_source_texture(f,w,h);
        synthetic[i]=create_source_texture(f,w,h);
        sp[i]=source[i].Get(); cp[i]=current[i].Get(); yp[i]=synthetic[i].Get();
    }
    auto history=std::make_shared<xrfg::D3D12SwapchainHistory>();
    require_hresult(history->initialize(f.device(),f.queue(),sp,D3D12_RESOURCE_STATE_RENDER_TARGET),"history");
    xrfg::D3D12FrameSynthesizer synth;
    require_hresult(synth.initialize(f.device(),f.queue(),history,cp,yp,kFormat,
        D3D12_RESOURCE_STATE_RENDER_TARGET,xrfg::D3D12OpticalFlowBackend::nvidia),"synth");
    const auto views=make_reprojection_views();
    auto capture=[&](unsigned frame) {
        upload_pattern(f,source[frame%3].Get(),translated_motion_pattern(w,h,{int(frame)*20,-int(frame)*20}));
        xrfg::D3D12HistoryCaptureTicket t{};
        require_hresult(history->capture(frame%3,&t),"capture");
        require_hresult(history->commit(t),"commit");
        return t;
    };
    auto a=capture(0);
    xrfg::D3D12FrameSynthesisTicket out{};
    require_hresult(synth.submit_prime(a,views,0,&out),"prime");
    auto b=capture(1);
    require_hresult(synth.submit_pair(b,views,views,0,1,&out),"pair");
    const auto real=readback_pattern(f,current[1].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto previous=real;
    for (unsigned slot=1; slot<=5; ++slot) {
        const float alpha=slot*0.2F;
        require_hresult(synth.submit_cover(b.serial,alpha,slot%3,&out),"dynamic cover");
        require(out.previous_serial==b.serial && out.current_serial==b.serial,"generated source became anchor");
        auto image=readback_pattern(f,synthetic[slot%3].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
        require(image!=previous,"different output times repeated the same image");
        const auto future=translated_motion_pattern(w,h,{20+int(slot)*4,-20-int(slot)*4});
        for (unsigned eye=0; eye<2; ++eye) {
            const auto predicted=mean_absolute_rgb_error(image,future,w,h,eye,48);
            const auto held=mean_absolute_rgb_error(real,future,w,h,eye,48);
            std::cout<<"DYNAMIC_GPU slot="<<slot<<" eye="<<eye<<" predicted_mae="<<predicted<<" held_mae="<<held<<'\n';
            require(predicted < held*0.85,"prediction failed opposing stereo motion");
        }
        require(synth.submit_cover(b.serial,alpha,0,&out)==E_INVALIDARG,"duplicate fraction accepted");
        previous=std::move(image);
    }
    require(readback_pattern(f,current[1].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET)==real,"real output modified");
    require(synth.submit_cover(b.serial,1.01F,0,&out)==E_INVALIDARG,"horizon >1 accepted");
    require(synth.submit_cover(b.serial,std::nanf(""),0,&out)==E_INVALIDARG,"NaN accepted");
    unsigned covers=5;
    for (unsigned frame=2; frame<20; ++frame) {
        const auto t=capture(frame);
        require_hresult(synth.submit_pair(t,views,views,0,0,&out),"rolling pair");
        static_cast<void>(readback_pattern(f,current[0].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET));
        require(synth.submit_cover(t.serial-1,0.1F,0,&out)==E_INVALIDARG,"old pair flow reused");
        for (unsigned slot=1; slot<=5; ++slot) {
            require_hresult(synth.submit_cover(t.serial,slot*0.1F,slot%3,&out),"rolling cover");
            static_cast<void>(readback_pattern(f,synthetic[slot%3].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET));
            ++covers;
        }
        require(synth.submit_cover(t.serial,0.6F,0,&out)==E_INVALIDARG,"sixth cover accepted");
    }
    const auto t=capture(20);
    require_hresult(synth.submit_pair(t,views,views,0,0,&out),"busy pair");
    static_cast<void>(readback_pattern(f,current[0].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET));
    ComPtr<ID3D12Fence> blocker;
    require_hresult(f.device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&blocker)),"blocker");
    require_hresult(f.queue()->Wait(blocker.Get(),1),"queue gate");
    const HRESULT first=synth.submit_cover(t.serial,0.2F,1,&out);
    const auto started=std::chrono::steady_clock::now();
    const HRESULT busy=synth.submit_cover(t.serial,0.4F,2,&out);
    const auto wait_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    // Always release the test gate before asserting so a failure still drains.
    require_hresult(blocker->Signal(1),"open gate");
    require_hresult(first,"first gated cover");
    require(busy==HRESULT_FROM_WIN32(ERROR_BUSY) && wait_ms<10,"busy GPU queued or blocked optional work");
    static_cast<void>(readback_pattern(f,synthetic[1].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET));
    require_hresult(synth.submit_cover(t.serial,0.4F,2,&out),"busy retry consumed fraction");
    static_cast<void>(readback_pattern(f,synthetic[2].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET));
    require_hresult(synth.retire_previous(),"retire");
    require(synth.submit_cover(t.serial,0.6F,0,&out)==E_INVALIDARG,"retired anchor accepted");
    require_hresult(history->invalidate(),"invalidate");
    require_hresult(history->wait_for_idle(),"drain");
    f.require_no_debug_errors();
    std::cout<<"DYNAMIC_GPU_PASS stereo_predictions=10 rolling_covers="<<covers
        <<" busy_nonblocking=1 debug_errors=0\n";
}

int main() {
    try { D3D12WarpFixture f(true); dynamic_cover_tests(f); return 0; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
