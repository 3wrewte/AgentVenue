#pragma once

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
    void comb(const MarketView& future_view, SignalSet& out) override {
        out.tick  = future_view.tick;
        out.valid = future_view.valid;

        for (size_t v = 0; v < MAX_VENUES; ++v) {
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
};

} // namespace av
