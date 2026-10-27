#ifdef XRFG_BASELINE_POLICY
#include XRFG_BASELINE_POLICY
#else
#include "xrfg/late_cover_policy.hpp"
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

void require_policy(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        constexpr std::int64_t second = 1'000'000'000;
        const auto p = xrfg::kLate60Period;
        for (const auto fps : {30, 24, 23, 20, 17, 16, 15, 10}) {
            unsigned real_count = 0, generated = 0, held = 0, last_slot = 0, max_slots = 0;
            std::int64_t anchor = 0;
            for (std::int64_t tick = 0; tick < 1200; ++tick) {
                const auto now = tick * second / 60;
                if (now >= static_cast<std::int64_t>(real_count) * second / fps) {
                    ++real_count;
                    anchor = now;
                    last_slot = 0;
                } else {
                    const auto d = xrfg::decide_late_cover(true, false, last_slot,
                        now - anchor, p, second / fps);
                    if (d.generate) {
                        ++generated;
                        last_slot = static_cast<unsigned>((now - anchor + p / 2) / p);
                        max_slots = std::max(max_slots, last_slot);
                        require_policy(std::isfinite(d.fraction) && d.fraction > 0 && d.fraction <= 1,
                            "invalid prediction fraction");
                        require_policy(!xrfg::decide_late_cover(true, false, last_slot,
                            now - anchor, p, second / fps).generate, "same-slot burst");
                    } else ++held;
                }
            }
            require_policy(real_count == static_cast<unsigned>(fps * 20), "source cadence altered");
#ifdef XRFG_BASELINE_POLICY
            require_policy(generated <= real_count && max_slots <= 1, "baseline contract changed");
            require_policy(held > 0 || fps == 30, "baseline unexpectedly fills low source");
#else
            require_policy(generated == static_cast<unsigned>((60 - fps) * 20) && held == 0,
                "fractional source did not fill nominal output slots");
#endif
            std::cout << "POLICY fps=" << fps << " real=" << real_count << " generated=" << generated
                << " held=" << held << " max_slot=" << max_slots << '\n';
        }
        require_policy(!xrfg::decide_late_cover(true, true, 0, p, p, 2*p).generate, "real work lost priority");
        require_policy(!xrfg::decide_late_cover(false, false, 0, p, p, 2*p).generate, "missing source accepted");
        require_policy(!xrfg::decide_late_cover(true, false, 0, 20*p, p, 2*p).generate, "unbounded prediction");
        require_policy(!xrfg::decide_late_cover(true, false, 0, p, 0, 2*p).generate, "zero output period");
#ifndef XRFG_BASELINE_POLICY
        const auto jumped = xrfg::decide_late_cover(true, false, 1, 4*p, p, 6*p);
        require_policy(jumped.generate && jumped.slot == 4, "missed slots queued instead of skipped");
        require_policy(!xrfg::decide_late_cover(true, false, 5, 6*p, p, 6*p).generate, "sixth cover accepted");
        const auto delayed = xrfg::decide_late_cover(true, false, 4, 5*p+p/3, p, 12*p);
        require_policy(delayed.generate && delayed.slot == 5 && delayed.fraction <= 5.0F/12.0F,
            "half-slot scheduling tolerance extended the prediction horizon");
        float last = 0;
        for (unsigned slot=1; slot<=5; ++slot) {
            const auto d = xrfg::decide_late_cover(true, false, slot-1, slot*p, p, 6*p);
            require_policy(d.generate && d.slot == slot && d.fraction > last, "nonmonotonic prediction");
            last = d.fraction;
        }
#endif
        std::cout << "DYNAMIC_POLICY_PASS mode="
#ifdef XRFG_BASELINE_POLICY
            << "baseline single_cover=1\n";
#else
            << "modified fractional_rates=8 no_bursts=1 bounded=1\n";
#endif
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
