#pragma once

namespace xrfg {
// Called outside loader lock, before Unity creates its application device.
// Only the opt-in asynchronous late60 path enables this process-local hook.
[[nodiscard]] bool enable_d3d11_device_threading() noexcept;
}
