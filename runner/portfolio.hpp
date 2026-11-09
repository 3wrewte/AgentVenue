#pragma once

#include <algorithm>
#include <cstddef>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "agents/agent_types.hpp"

namespace av {

// Spot-style, contract multiplier = 1. Position is signed; cash is in
// price*qty units (buy -> cash decreases, sell -> cash increases). Average
// cost and PnL are doubles for readability (AgentVenue is not the RTL core).
struct PortfolioState {
    std::array<VenuePortfolio, MAX_VENUES> venues{};

    void reset() { venues = {}; }

    static void apply_fill(VenuePortfolio& v, Side_t side, Price_t price, Qty_t qty) {
        const int64_t q = (side == Side_t::Buy) ? static_cast<int64_t>(qty)
                                                : -static_cast<int64_t>(qty);
        const double px = static_cast<double>(price);

        v.cash -= q * static_cast<int64_t>(price);

        if (v.position == 0 || ((v.position > 0) == (q > 0))) {
            const int64_t old_abs = std::llabs(v.position);
            const int64_t add_abs = std::llabs(q);
            const int64_t new_abs = old_abs + add_abs;
            v.avg_cost = new_abs == 0
                             ? 0.0
                             : (v.avg_cost * static_cast<double>(old_abs) +
                                px * static_cast<double>(add_abs)) /
                                   static_cast<double>(new_abs);
            v.position += q;
            return;
        }

        const int64_t pos_abs  = std::llabs(v.position);
        const int64_t fill_abs = std::llabs(q);
        const int64_t matched  = std::min(pos_abs, fill_abs);
        const double  sign     = (v.position > 0) ? 1.0 : -1.0;

        const int64_t old_position = v.position;
        v.realized_pnl += (px - v.avg_cost) * static_cast<double>(matched) * sign;
        v.position += q;

        if (v.position == 0) {
            v.avg_cost = 0.0;
        } else if ((old_position > 0) != (v.position > 0)) {
            // Position flipped through zero: the remainder is a fresh
            // position opened at this fill price.
            v.avg_cost = px;
        }
    }

    // Mark-to-market over all venues using the given per-venue marks.
    void mark(const std::array<double, MAX_VENUES>& marks,
              const std::array<bool, MAX_VENUES>& valid) {
        for (size_t v = 0; v < MAX_VENUES; ++v) {
            if (valid[v]) {
                venues[v].unrealized_pnl =
                    static_cast<double>(venues[v].position) *
                    (marks[v] - venues[v].avg_cost);
            } else {
                venues[v].unrealized_pnl = 0.0;
            }
        }
    }
};

} // namespace av
