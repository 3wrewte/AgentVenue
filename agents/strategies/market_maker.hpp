#pragma once

// Two-sided market maker.
//
// The quote centre is the delayed mid (the same view the agent receives) with
// an inventory skew:
//
//   reservation = mid - gamma * inventory
//   bid         = reservation - half_spread
//   ask         = reservation + half_spread
//
// The bid/ask are clamped to stay on the maker side of the *delayed* book. The
// order still reaches the exchange one tick later, so a moving book can pick
// the quote off; that latency risk is part of the model.
//
// Quotes are not replaced every tick: they rest up to `max_rest_ticks` and are
// then cancelled and re-quoted, which keeps the churn and the queue-position
// trade-off under one parameter. `half_spread` is drawn per agent from
// N(mean, spread_sigma) at reset, so s1 is cross-sectional heterogeneity.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>

#include "agents/agent_base.hpp"
#include "agents/agent_types.hpp"
#include "agents/strategies/strategy_exec.hpp"

namespace av {

class MarketMakerStrategy : public AgentBase {
public:
    struct Config {
        double   half_spread    = 2.0;   // mean p1, in ticks
        double   spread_sigma   = 0.0;   // s1, across-agent std dev
        Qty_t    lot            = 10;
        Qty_t    max_inventory  = 1000;  // B (signed inventory cap)
        double   gamma          = 0.0;   // inventory skew, ticks per unit
        int      max_rest_ticks = 5;
        uint8_t  venue          = strat::kRealVenue;
        Sym_t    sym            = 1;
        uint64_t seed           = 0;     // drives the half_spread draw
    };

    Config cfg;
    ClientOrderId next_id = 1;

    MarketMakerStrategy() = default;

    MarketMakerStrategy(uint32_t id_, AccountId account_, uint32_t class_id_, Config c)
        : cfg(c) {
        this->id       = id_;
        this->account  = account_;
        this->class_id = class_id_;
    }

    void on_reset() override {
        next_id = 1;
        rest_ticks_ = 0;

        std::mt19937_64 rng(cfg.seed);
        std::normal_distribution<double> dist(0.0, 1.0);
        const double jitter = cfg.spread_sigma > 0.0
                                  ? cfg.spread_sigma * dist(rng)
                                  : 0.0;
        half_spread_ = std::max(1.0, cfg.half_spread + jitter);
    }

    void comb() override {
        if (static_cast<size_t>(cfg.venue) >= MAX_VENUES) return;
        if (!mkt_in.valid) {
            cancel_all();
            return;
        }

        const VenueView& v = mkt_in.venues[cfg.venue];
        if (v.bid_price == 0 || v.ask_price == 0) {
            cancel_all();
            return;
        }

        if (!priv_in.open_orders.empty()) {
            if (++rest_ticks_ > cfg.max_rest_ticks) {
                strat::cancel_resting(*this, cfg.sym, cfg.venue);
                rest_ticks_ = 0;
            }
            return;
        }
        rest_ticks_ = 0;

        double mid = 0.5 * (static_cast<double>(v.bid_price) +
                            static_cast<double>(v.ask_price));
        if (signals_in.valid && signals_in.mid_valid[cfg.venue]) {
            mid = static_cast<double>(signals_in.mid[cfg.venue]);
        }

        const int64_t inv = portfolio_in.venues[cfg.venue].position;
        const int64_t max_inv = static_cast<int64_t>(cfg.max_inventory);

        const double reservation = mid - cfg.gamma * static_cast<double>(inv);
        const int64_t hs = std::max<int64_t>(
            1, std::llround(half_spread_));

        int64_t bid = std::llround(reservation) - hs;
        int64_t ask = std::llround(reservation) + hs;

        // Stay a maker against the delayed book.
        bid = std::min<int64_t>(bid, static_cast<int64_t>(v.ask_price) - 1);
        ask = std::max<int64_t>(ask, static_cast<int64_t>(v.bid_price) + 1);

        bid = static_cast<int64_t>(strat::clamp_price(bid));
        ask = static_cast<int64_t>(strat::clamp_price(ask));
        if (bid >= ask) return;

        int64_t bid_qty = 0;
        int64_t ask_qty = 0;
        if (inv < max_inv) {
            bid_qty = std::min<int64_t>(static_cast<int64_t>(cfg.lot),
                                        max_inv - inv);
        }
        if (inv > -max_inv) {
            ask_qty = std::min<int64_t>(static_cast<int64_t>(cfg.lot),
                                        inv + max_inv);
        }

        if (bid_qty > 0) {
            strat::push_order(out_next, cfg.venue, cfg.sym, Side_t::Buy,
                              static_cast<Price_t>(bid),
                              static_cast<Qty_t>(bid_qty), next_id++);
        }
        if (ask_qty > 0) {
            strat::push_order(out_next, cfg.venue, cfg.sym, Side_t::Sell,
                              static_cast<Price_t>(ask),
                              static_cast<Qty_t>(ask_qty), next_id++);
        }
    }

    double half_spread() const { return half_spread_; }

private:
    double half_spread_ = 2.0;
    int    rest_ticks_  = 0;

    void cancel_all() {
        if (priv_in.open_orders.empty()) return;
        strat::cancel_resting(*this, cfg.sym, cfg.venue);
        rest_ticks_ = 0;
    }
};

} // namespace av
