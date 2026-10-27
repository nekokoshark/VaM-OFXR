#define main upstream_history_matrix_main
#include "d3d12_history_tests.cpp"
#undef main
#include "xrfg/late_cover_policy.hpp"

void policy_tests() {
    const auto p = xrfg::kLate60Period;
    require(xrfg::late_output_period(8'333'333) == p, "120Hz target not capped at 60");
    require(xrfg::late_output_period(22'222'222) == 22'222'222, "target exceeds runtime limit");
    require(xrfg::decide_late_cover(true, false, false, p, p, 2*p).generate, "late slot not covered");
    require(xrfg::decide_late_cover(true, false, false, p, p, 2*p).fraction == 0.5F, "wrong horizon");
    require(!xrfg::decide_late_cover(true, true, false, p, p, 2*p).generate, "cover overtook real work");
    require(!xrfg::decide_late_cover(true, false, true, p, p, 2*p).generate, "recursive cover");
    require(!xrfg::decide_late_cover(false, false, false, p, p, 2*p).generate, "unprimed cover");
    require(!xrfg::decide_late_cover(true, false, false, p/3, p, 2*p).generate, "premature cover");
    require(!xrfg::decide_late_cover(true, false, false, 6*p, p, 2*p).generate, "stale flow cover");
    require(!xrfg::decide_late_cover(true, false, false, p, p, 15*p).generate, "slow source extrapolated indefinitely");
}

void cover_tests(D3D12WarpFixture& f) {
    constexpr UINT w = 1024, h = 512;
    std::array<ComPtr<ID3D12Resource>,3> source, current, synthetic;
    std::array<ID3D12Resource*,3> sp{}, cp{}, yp{};
    for (unsigned i=0; i<3; ++i) {
        source[i]=create_source_texture(f,w,h);
        current[i]=create_source_texture(f,w,h);
        synthetic[i]=create_source_texture(f,w,h);
        sp[i]=source[i].Get(); cp[i]=current[i].Get(); yp[i]=synthetic[i].Get();
    }
    auto history=std::make_shared<xrfg::D3D12SwapchainHistory>();
    require_hresult(history->initialize(f.device(),f.queue(),sp,D3D12_RESOURCE_STATE_RENDER_TARGET),"history initialize");
    xrfg::D3D12FrameSynthesizer synth;
    require_hresult(synth.initialize(f.device(),f.queue(),history,cp,yp,kFormat,
        D3D12_RESOURCE_STATE_RENDER_TARGET,xrfg::D3D12OpticalFlowBackend::nvidia),"synthesis initialize");
    const auto views=make_reprojection_views();
    auto capture=[&](unsigned i, const StereoPattern& pattern) {
        upload_pattern(f,source[i].Get(),pattern);
        xrfg::D3D12HistoryCaptureTicket t{};
        require_hresult(history->capture(i,&t),"capture");
        require_hresult(history->commit(t),"commit");
        return t;
    };
    const auto a=translated_motion_pattern(w,h,{0,0});
    const auto b=translated_motion_pattern(w,h,{16,-16});
    const auto future=translated_motion_pattern(w,h,{24,-24});
    auto ta=capture(0,a);
    xrfg::D3D12FrameSynthesisTicket out{};
    require(synth.submit_cover(ta.serial,0.5F,0,&out)==E_INVALIDARG,"unprimed cover accepted");
    require_hresult(synth.submit_prime(ta,views,0,&out),"prime");
    auto tb=capture(1,b);
    require_hresult(synth.submit_pair(tb,views,views,0,1,&out),"pair");
    static_cast<void>(readback_pattern(f,current[1].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET));
    require(synth.submit_cover(tb.serial+1,0.5F,1,&out)==E_INVALIDARG,"stale serial accepted");
    require(synth.submit_cover(tb.serial,1.1F,1,&out)==E_INVALIDARG,"unbounded horizon accepted");
    require(synth.submit_cover(tb.serial,std::nanf(""),1,&out)==E_INVALIDARG,"NaN accepted");
    const HRESULT cover_result = synth.submit_cover(tb.serial,0.5F,1,&out);
    if (FAILED(cover_result)) f.require_no_debug_errors();
    require_hresult(cover_result,"late cover");
    require(out.previous_serial==tb.serial && out.current_serial==tb.serial,"cover not anchored on B");
    auto image=readback_pattern(f,synthetic[1].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
    require(image!=b,"cover just repeated B");
    require(readback_pattern(f,current[1].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET)==b,"cover overwrote real output");
    for (unsigned eye=0; eye<2; ++eye) {
        const auto predicted=mean_absolute_rgb_error(image,future,w,h,eye,48);
        const auto held=mean_absolute_rgb_error(b,future,w,h,eye,48);
        std::cout<<"COVER eye="<<eye<<" predicted_mae="<<predicted<<" held_mae="<<held<<'\n';
        require(predicted < held*0.7,"cover did not follow measured motion independently per eye");
    }
    require(synth.submit_cover(tb.serial,0.5F,2,&out)==E_INVALIDARG,"second cover from same pair accepted");
    // More than a ring's worth of real frames, each followed by a cover: this
    // detects missing B consumer-fence extension and retired-source reuse.
    for (unsigned frame=2;frame<20;++frame) {
        auto t=capture(frame%3,translated_motion_pattern(w,h,{int(frame)*16,-int(frame)*16}));
        require_hresult(synth.submit_pair(t,views,views,0,0,&out),"rolling pair");
        static_cast<void>(readback_pattern(f,current[0].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET));
        require_hresult(synth.submit_cover(t.serial,0.25F,1,&out),"rolling cover");
        static_cast<void>(readback_pattern(f,synthetic[1].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET));
    }
    require_hresult(synth.retire_previous(),"retire");
    require(synth.submit_cover(20,0.5F,1,&out)==E_INVALIDARG,"retired source accepted");
    require_hresult(history->invalidate(),"invalidate");
    require_hresult(history->wait_for_idle(),"drain history");
    f.require_no_debug_errors();
}

int main() {
    try {
        policy_tests();
        D3D12WarpFixture hardware(true);
        cover_tests(hardware);
        xrfg::D3D12NvidiaOpticalFlowOptions options;
        test_stereo_motion_synthesis_beats_same_pixel_blend(hardware,
            xrfg::D3D12OpticalFlowBackend::nvidia,true,options);
        test_double_wide_single_slice_views(hardware,xrfg::D3D12OpticalFlowBackend::nvidia,options);
        test_rotation_aware_synthesis_beats_uncompensated_flow(hardware,xrfg::D3D12OpticalFlowBackend::nvidia,options);
        hardware.require_no_debug_errors();
        std::cout<<"LATE60_GPU_PASS policy=10 stereo_motion=2 rolling_covers=19 default_2x=3 debug_errors=0\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
