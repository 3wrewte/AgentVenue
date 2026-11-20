#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "agents/agent_types.hpp"
#include "agents/signal_base.hpp"

namespace av {

// First standard signal module: mid and spread per venue.
//
// It reads the MarketView that the agents will receive on the next tick, so
// its output can be registered for one cycle and delivered together with that
// same view. No ordering constraint between signal computation and agent comb.
class StandardSignals : public SignalModule {
public:
    StandardSignals() = default;

    StandardSignals(bool fundamental_enabled, double fundamental_base,
                    double fundamental_growth,
                    uint64_t fundamental_jump_period = 0,
                    double fundamental_jump_size = 0.0,
                    uint64_t fundamental_seed = 0,
                    bool fundamental_per_venue = false)
        : fundamental_enabled_(fundamental_enabled),
          fundamental_base_(fundamental_base),
          fundamental_growth_(fundamental_growth),
          fundamental_jump_period_(fundamental_jump_period),
          fundamental_jump_size_(fundamental_jump_size),
          fundamental_seed_(fundamental_seed),
          fundamental_per_venue_(fundamental_per_venue) {}

    void comb(const MarketView& future_view, SignalSet& out) override {
        out.tick  = future_view.tick;
        out.valid = future_view.valid;

        for (size_t v = 0; v < MAX_VENUES; ++v) {
            out.fundamental_valid[v] = false;
            out.fundamental[v] = 0;
            if (fundamental_enabled_) {
                const uint64_t seed = fundamental_per_venue_
                    ? fundamental_seed_ + 0x9e3779b97f4a7c15ull * (v + 1)
                    : fundamental_seed_;
                const double raw = fundamental_value(future_view.tick, seed);
                const long long px = static_cast<long long>(std::llround(raw));
                out.fundamental[v] = static_cast<Price_t>(
                    std::clamp<long long>(px, 1, Price_max));
                out.fundamental_valid[v] = true;
            }

            const VenueView& vv = future_view.venues[v];
            if (vv.bid_price != 0 && vv.ask_price != 0) {
                out.mid[v] = static_cast<Price_t>(
                    (static_cast<uint64_t>(vv.bid_price) +
                     static_cast<uint64_t>(vv.ask_price)) / 2);
                out.mid_valid[v] = true;
                out.spread[v] = vv.ask_price > vv.bid_price
                                    ? static_cast<Qty_t>(vv.ask_price - vv.bid_price)
                                    : 0;
            } else {
                out.mid[v] = 0;
                out.mid_valid[v] = false;
                out.spread[v] = 0;
            }
        }
    }

private:
    static uint64_t mix64(uint64_t x) {
        x += 0x9e3779b97f4a7c15ull;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
        return x ^ (x >> 31);
    }

    static double unit01(uint64_t x) {
        return static_cast<double>(x >> 11) *
               (1.0 / 9007199254740992.0);  // 2^53
    }

    double fundamental_value(uint64_t tick, uint64_t seed) const {
        if (fundamental_jump_period_ == 0) {
            return fundamental_base_ *
                   std::exp(fundamental_growth_ * static_cast<double>(tick));
        }
        double value = fundamental_base_;
        const uint64_t seg = tick / fundamental_jump_period_;
        const uint64_t rem = tick % fundamental_jump_period_;
        for (uint64_t k = 0; k <= seg; ++k) {
            const uint64_t h = mix64(seed + 0x9e3779b97f4a7c15ull * (k + 1));
            if (k > 0) {
                // A sharp multiplicative jump at the regime boundary.
                const double z = 2.0 * unit01(h) - 1.0;
                value *= std::exp(fundamental_jump_size_ * z);
            }
            // Random drift direction inside the regime.
            const int sign = (h & 1ull) ? 1 : -1;
            const uint64_t dt = (k == seg) ? rem : fundamental_jump_period_;
            value *= std::exp(fundamental_growth_ * static_cast<double>(sign) *
                              static_cast<double>(dt));
        }
        return value;
    }

    bool     fundamental_enabled_ = false;
    double   fundamental_base_   = REF_ANCHOR_PRICE;
    double   fundamental_growth_ = 0.0;
    uint64_t fundamental_jump_period_ = 0;
    double   fundamental_jump_size_ = 0.0;
    uint64_t fundamental_seed_ = 0;
    bool     fundamental_per_venue_ = false;
};

} // namespace av
