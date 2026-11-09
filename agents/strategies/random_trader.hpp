#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <random>
#include <vector>

#include "agents/agent_base.hpp"
#include "agents/agent_types.hpp"

namespace av {

// Noise trader used to drive the first simulation and to stress the venue.
//
// Each tick:
//   * n_new    ~ Poisson(lambda_new)    new orders (capped by max_open)
//   * n_cancel ~ Poisson(lambda_cancel) cancels of currently resting orders
// New prices are drawn from N(anchor_or_mid, sigma); quantities are uniform in
// [qty_min, qty_max] and capped at MAX_AGENT_QTY. Most orders are limit orders.
class RandomTrader : public AgentBase {
public:
    struct Config {
        double  lambda_new    = 0.10;
        double  lambda_cancel = 0.05;
        double  sigma         = 5.0;
        double  p_buy         = 0.5;
        uint32_t qty_min      = 1;
        uint32_t qty_max      = 100;
        double  limit_ratio   = 0.9;
        size_t  max_open      = 8;
        Price_t anchor        = REF_ANCHOR_PRICE;
        uint8_t venue         = 0;
        Sym_t   sym           = 1;
    };

    Config cfg;
    ClientOrderId next_client_id = 1;

    RandomTrader() : rng_(0x9e3779b97f4a7c15ull) {}
    RandomTrader(uint32_t id, AccountId account, uint32_t class_id,
                 Config config, uint64_t seed)
        : cfg(config), seed_(seed), rng_(seed) {
        this->id = id;
        this->account = account;
        this->class_id = class_id;
    }

    void set_seed(uint64_t seed) { seed_ = seed; rng_.seed(seed); }

    void on_reset() override {
        rng_.seed(seed_);
        next_client_id = 1;
    }

    void comb() override {
        // Reference point: delayed mid when the book has both sides, otherwise
        // the fixed anchor (which is also the synthetic reference price).
        Price_t mid = cfg.anchor;
        const VenueView& vv = mkt_in.venues[cfg.venue];
        if (signals_in.valid && signals_in.mid_valid[cfg.venue]) {
            // Use the precomputed signal: it was derived from exactly the
            // MarketView delivered above (computed one cycle earlier).
            mid = signals_in.mid[cfg.venue];
        } else if (vv.bid_price != 0 && vv.ask_price != 0) {
            mid = static_cast<Price_t>(
                (static_cast<uint64_t>(vv.bid_price) +
                 static_cast<uint64_t>(vv.ask_price)) / 2);
        }

        const size_t n_open = priv_in.open_orders.size();

        // ---- cancels ----
        int n_cancel = std::poisson_distribution<int>(cfg.lambda_cancel)(rng_);
        if (n_cancel > 0 && n_open > 0) {
            std::vector<size_t> idx(n_open);
            for (size_t i = 0; i < n_open; ++i) idx[i] = i;
            std::shuffle(idx.begin(), idx.end(), rng_);
            const size_t take = std::min<size_t>(
                {static_cast<size_t>(n_cancel), n_open, MAX_ORDERS_PER_TICK});
            for (size_t k = 0; k < take; ++k) {
                const OpenOrder& oo = priv_in.open_orders[idx[k]];
                AgentOrder o{};
                o.venue = oo.venue;
                o.kind = OPKind::Cancel;
                o.sym = cfg.sym;
                o.side = oo.side;
                o.client_id = oo.client_id;
                out_next.push(o);
            }
        }

        // ---- new orders ----
        int n_new = std::poisson_distribution<int>(cfg.lambda_new)(rng_);
        const size_t room = out_next.n < MAX_ORDERS_PER_TICK
                                ? MAX_ORDERS_PER_TICK - out_next.n
                                : 0;
        const size_t open_room = n_open < cfg.max_open ? cfg.max_open - n_open : 0;
        const size_t budget = std::min(room, open_room);
        const size_t count = std::min<size_t>(n_new < 0 ? 0 : static_cast<size_t>(n_new),
                                              budget);

        std::normal_distribution<double> price_dist(static_cast<double>(mid),
                                                    cfg.sigma);
        const uint32_t qmax = std::min<uint32_t>(cfg.qty_max, MAX_AGENT_QTY);
        const uint32_t qmin = std::min<uint32_t>(cfg.qty_min, qmax);
        std::uniform_int_distribution<uint32_t> qty_dist(qmin, qmax);
        std::bernoulli_distribution side_dist(cfg.p_buy);
        std::bernoulli_distribution limit_dist(cfg.limit_ratio);

        for (size_t k = 0; k < count; ++k) {
            AgentOrder o{};
            o.venue = cfg.venue;
            o.sym = cfg.sym;
            o.side = side_dist(rng_) ? Side_t::Buy : Side_t::Sell;
            o.kind = limit_dist(rng_) ? OPKind::Limit : OPKind::Market;
            o.qty = qty_dist(rng_);
            o.client_id = next_client_id++;

            long long px = std::llround(price_dist(rng_));
            if (px < 1) px = 1;
            if (px > static_cast<long long>(Price_max)) px = Price_max;
            o.price = static_cast<Price_t>(px);

            out_next.push(o);
        }
    }

private:
    uint64_t seed_ = 0x9e3779b97f4a7c15ull;
    std::mt19937_64 rng_;
};

} // namespace av
