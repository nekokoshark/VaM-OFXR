#pragma once
#include <algorithm>
#include <cstdint>

namespace xrfg {
constexpr std::int64_t kLate60Period = 16'666'667;
constexpr std::uint32_t kLate60MaxCoverSlots = 5;
constexpr std::int64_t kLate60MaxPrediction = kLate60Period * kLate60MaxCoverSlots;
constexpr std::int64_t kLate60MaxSourceInterval = kLate60Period * 12;
constexpr std::int64_t kLate60CoverBudget = kLate60Period / 2;
struct LateCoverDecision {
    bool generate{};
    float fraction{};
    std::uint32_t slot{};
};

// Fill only the current missing output slot, not a fixed integer multiplier.
// Every prediction starts at the latest REAL endpoint. Never catch up in bursts
// or extend beyond one measured source interval / 83.3ms / five output slots.
inline LateCoverDecision decide_late_cover(bool pair_ready, bool real_arriving,
    std::uint32_t last_slot, std::int64_t since_real, std::int64_t period,
    std::int64_t real_interval) noexcept {
    if (!pair_ready || real_arriving || period <= 0 ||
        period > kLate60MaxSourceInterval || real_interval < period ||
        real_interval > kLate60MaxSourceInterval || since_real < period / 2 ||
        since_real > std::min(real_interval, kLate60MaxPrediction) + period / 2)
        return {};
    const auto slot = static_cast<std::uint32_t>((since_real + period / 2) / period);
    if (slot == 0 || slot <= last_slot || slot > kLate60MaxCoverSlots) return {};
    const auto prediction = std::min(since_real, kLate60MaxPrediction);
    return {true, static_cast<float>(std::min(1.0,
        static_cast<double>(prediction) / static_cast<double>(real_interval))), slot};
}
inline std::int64_t late_output_period(std::int64_t native) noexcept {
    return std::max(native, kLate60Period);
}
}
