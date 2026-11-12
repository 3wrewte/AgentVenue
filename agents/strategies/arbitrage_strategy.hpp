#pragma once

// Basic cross-venue arbitrage: real core (venue 0) vs synthetic reference
// market (venue 1, constant bid/ask, infinite liquidity).
//
//   real bid > ref ask  ->  buy  ref @ ref_ask , sell real @ real_bid
//   real ask < ref bid  ->  buy  real @ real_ask, sell ref  @ ref_bid
//
// Both legs are aggressive limits sent in the same `comb()`, so they both reach
// their venue on the next tick (framework reg-out delay). The pair is therefore
// NOT atomic: one leg can fill while the other is partially filled or rejected.
// `max_pos` caps the unhedged inventory left on each venue, and only one pair is
// in flight at a time.
//
// `edge` is the minimum cross (in ticks) required on top of the reference
// spread; 1 means "at least one tick of profit per unit before costs".

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>

#include "agents/agent_base.hpp"
#include "agents/agent_types.hpp"
#include "agents/strategies/strategy_exec.hpp"

namespace av {

class ArbitrageStrategy : public AgentBase {
public:
    struct Config {
        uint8_t venue_real = strat::kRealVenue;
        uint8_t venue_ref  = strat::kRefVenue;
        Sym_t   sym        = 1;
        Qty_t   lot        = 10;
        int64_t max_pos    = 30;  // |position| cap per venue
        int64_t edge       = 1;   // p2: mean minimum profit per unit, in ticks
        double  edge_sigma = 0.0; // s2: cross-sectional std dev of the edge
        uint64_t seed      = 0;   // drives the edge draw
    };

    Config cfg;
    ClientOrderId next_id = 1;
    int64_t edge_ = 1;

    ArbitrageStrategy() = default;

    ArbitrageStrategy(uint32_t id_, AccountId account_, uint32_t class_id_, Config c)
        : cfg(c) {
        this->id       = id_;
        this->account  = account_;
        this->class_id = class_id_;
    }

    void on_reset() override {
        next_id = 1;

        std::mt19937_64 rng(cfg.seed);
        std::normal_distribution<double> dist(0.0, 1.0);
        const double jitter = cfg.edge_sigma > 0.0
                                  ? cfg.edge_sigma * dist(rng)
                                  : 0.0;
        const double edge = static_cast<double>(cfg.edge) + jitter;
        edge_ = edge > 0.0 ? static_cast<int64_t>(std::llround(edge)) : 0;
    }

    void comb() override {
        if (static_cast<size_t>(cfg.venue_real) >= MAX_VENUES) return;
        if (static_cast<size_t>(cfg.venue_ref) >= MAX_VENUES) return;
        if (!mkt_in.valid) return;

        const VenueView& real = mkt_in.venues[cfg.venue_real];
        const VenueView& ref  = mkt_in.venues[cfg.venue_ref];
        if (real.bid_price == 0 || real.ask_price == 0 ||
            ref.bid_price == 0 || ref.ask_price == 0) {
            // One side is missing; cancel anything left before standing down.
            if (!priv_in.open_orders.empty()) {
                strat::cancel_resting(*this, cfg.sym, cfg.venue_real);
            }
            return;
        }

        // One pair in flight at a time: clear any leftover resting order first.
        if (!priv_in.open_orders.empty()) {
            strat::cancel_resting(*this, cfg.sym, cfg.venue_real);
            return;
        }
        if (out_next.n + 2 > MAX_ORDERS_PER_TICK) return;  // need two slots

        const Qty_t lot = std::min<Qty_t>(cfg.lot, MAX_AGENT_QTY);
        if (lot == 0) return;
        const int64_t q = static_cast<int64_t>(lot);

        const int64_t real_bid = static_cast<int64_t>(real.bid_price);
        const int64_t real_ask = static_cast<int64_t>(real.ask_price);
        const int64_t ref_bid  = static_cast<int64_t>(ref.bid_price);
        const int64_t ref_ask  = static_cast<int64_t>(ref.ask_price);

        const int64_t pos_real = portfolio_in.venues[cfg.venue_real].position;
        const int64_t pos_ref  = portfolio_in.venues[cfg.venue_ref].position;

        // A: the real bid is above the ref offer -> buy cheap on ref, sell dear
        //    on the real book. The ref leg must be priced at ref_ask or it is
        //    rejected with LimitNotCrossingRef.
        if (real_bid - ref_ask >= edge_ &&
            pos_ref + q <= cfg.max_pos &&
            pos_real - q >= -cfg.max_pos) {
            strat::push_order(out_next, cfg.venue_ref, cfg.sym, Side_t::Buy,
                              ref.ask_price, lot, next_id++);
            strat::push_order(out_next, cfg.venue_real, cfg.sym, Side_t::Sell,
                              real.bid_price, lot, next_id++);
            return;
        }

        // B: the real offer is below the ref bid -> buy on the real book, sell
        //    on ref. A ref sell must be priced at ref_bid to cross.
        if (ref_bid - real_ask >= edge_ &&
            pos_ref - q >= -cfg.max_pos &&
            pos_real + q <= cfg.max_pos) {
            strat::push_order(out_next, cfg.venue_ref, cfg.sym, Side_t::Sell,
                              ref.bid_price, lot, next_id++);
            strat::push_order(out_next, cfg.venue_real, cfg.sym, Side_t::Buy,
                              real.ask_price, lot, next_id++);
        }
    }
};

} // namespace av
