#include "xrfg/standalone_launcher.hpp"
#include "xrfg/implicit_layer.hpp"
#include <windows.h>
#include <fstream>
#include <iostream>

int main() {
    using namespace xrfg::standalone;
    const auto directory = std::filesystem::temp_directory_path() /
        (L"ofxr-ratio-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    bool valid = LauncherSettings{}.frame_generation_ratio == 2 &&
        xrfg::implicit_layer::read_frame_generation_ratio({}) == 2;
    for (unsigned ratio : {2U, 3U, 0U, 4U}) {
        LauncherSettings settings;
        settings.frame_generation_ratio = ratio;
        settings.backend = FlowBackend::nvidia;
        settings.nvidia_preset = NvidiaPerformancePreset::slow;
        settings.nvidia_input_scale = NvidiaInputScale::three_quarter;
        settings.nvidia_bidirectional = true;
        settings.diagnostics = true;
        settings.overlay_position = xrfg::FpsOverlayPosition::lower_left;
        const auto parsed = parse_settings(serialize_settings(settings));
        {
            std::ofstream ini(directory / L"ofxr_bridge.ini", std::ios::trunc);
            ini << build_runtime_ini(parsed);
        }
        valid &= parsed.frame_generation_ratio == (ratio == 3 ? 3U : 2U) &&
            xrfg::implicit_layer::read_frame_generation_ratio(directory) == parsed.frame_generation_ratio &&
            parsed.backend == settings.backend && parsed.nvidia_preset == settings.nvidia_preset &&
            parsed.nvidia_input_scale == settings.nvidia_input_scale &&
            parsed.nvidia_bidirectional && parsed.diagnostics &&
            parsed.overlay_position == settings.overlay_position;
    }
    for (const char* value : {"", "2", "3", "4", "-1", "3junk", "999999999999"}) {
        {
            std::ofstream ini(directory / L"ofxr_bridge.ini", std::ios::trunc);
            ini << "[ofxr]\nframe_generation_ratio=" << value << '\n';
        }
        const unsigned expected = std::string_view(value) == "3" ? 3U : 2U;
        valid &= xrfg::implicit_layer::read_frame_generation_ratio(directory) == expected;
        valid &= parse_settings(std::string("[tray]\nframe_generation_ratio=") + value).frame_generation_ratio == expected;
    }
    std::filesystem::remove(directory / L"ofxr_bridge.ini");
    std::filesystem::remove(directory);
    if (!valid) {
        std::cerr << "frame generation settings failed\n";
        return 1;
    }
    std::cout << "2x/3x settings round-trip, invalid input and existing settings preservation passed\n";
    return 0;
}
