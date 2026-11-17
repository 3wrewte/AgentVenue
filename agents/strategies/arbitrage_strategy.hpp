#pragma once

// Basic cross-venue arbitrage between two real LOBs (venue 0 and venue 1).
//
//   venue0 bid > venue1 ask  ->  buy venue1 @ ask, sell venue0 @ bid   (A)
//   venue1 bid > venue0 ask  ->  buy venue0 @ ask, sell venue1 @ bid   (B)
//
// The pair is NOT atomic: either leg may only partially fill (or rest). The
// strategy is therefore position-aware:
//
//   1. leftover orders on BOTH venues are cancelled;
//   2. an unmatched net exposure (pos_real + pos_ref) is flattened with one
//      taker order on the leg that can reduce it;
//   3. a matched pair is exited when the round-trip PnL (using the portfolio's
//      average costs) is at least `exit_profit`, or after `max_hold_ticks`;
//   4. only when flat does it open a new pair with `edge` (p2).
//
// Entries are sized by the remaining position room, each leg's visible touch
// quantity, and each venue checker's available base/cash, so one leg is less
// likely to fill while the other is rejected.
//
// Risk-reducing orders ignore the `max_pos` caps; they are always allowed.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
        int64_t max_pos    = 30;  // |position| cap per venue for entries
        int64_t edge       = 1;   // p2: mean minimum entry edge, in ticks
        double  edge_sigma = 0.0; // s2: cross-sectional std dev of the edge
        uint64_t seed      = 0;   // drives the edge draw

        double exit_profit    = 0.0; // per-unit round-trip PnL needed to exit
        int    max_hold_ticks = 100; // force an exit after this many ticks
        Qty_t  hedge_gap      = 1;   // flatten net exposure >= this
        bool   use_visible_qty = true; // cap the real leg by the touch qty
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
        hold_ticks_ = 0;

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
        if (!mkt_in.valid) {
            cancel_open();
            return;
        }

        const VenueView& real = mkt_in.venues[cfg.venue_real];
        const VenueView& ref  = mkt_in.venues[cfg.venue_ref];
        if (real.bid_price == 0 || real.ask_price == 0 ||
            ref.bid_price == 0 || ref.ask_price == 0) {
            cancel_open();
            return;
        }

        // One pair in flight at a time: clear leftovers on BOTH venues first.
        if (!priv_in.open_orders.empty()) {
            cancel_open();
            return;
        }
        if (out_next.n + 2 > MAX_ORDERS_PER_TICK) return;

        const Qty_t lot = std::min<Qty_t>(cfg.lot, MAX_AGENT_QTY);
        if (lot == 0) return;

        const int64_t pos_real = portfolio_in.venues[cfg.venue_real].position;
        const int64_t pos_ref  = portfolio_in.venues[cfg.venue_ref].position;
        const double  avg_real = portfolio_in.venues[cfg.venue_real].avg_cost;
        const double  avg_ref  = portfolio_in.venues[cfg.venue_ref].avg_cost;

        const bool has_pos = (pos_real != 0 || pos_ref != 0);
        hold_ticks_ = has_pos ? hold_ticks_ + 1 : 0;

        // 1) Flatten the unmatched net exposure (hedge error).
        const int64_t net = pos_real + pos_ref;
        const int64_t gap = static_cast<int64_t>(
            cfg.hedge_gap == 0 ? 1 : cfg.hedge_gap);
        if (std::llabs(net) >= gap) {
            const Qty_t want = static_cast<Qty_t>(
                std::min<int64_t>(static_cast<int64_t>(lot), std::llabs(net)));
            if (net > 0 && pos_real > 0) {
                const Qty_t q = cap_sell(cfg.venue_real, want,
                                         real.bid_qty, real.bid_price);
                if (q) { push_sell_real(q, real.bid_price); return; }
            } else if (net < 0 && pos_real < 0) {
                const Qty_t q = cap_buy(cfg.venue_real, want,
                                        real.ask_qty, real.ask_price);
                if (q) { push_buy_real(q, real.ask_price); return; }
            } else if (net > 0 && pos_ref > 0) {
                const Qty_t want_ref = static_cast<Qty_t>(
                    std::min<int64_t>(static_cast<int64_t>(want), pos_ref));
                const Qty_t q = cap_sell(cfg.venue_ref, want_ref,
                                         ref.bid_qty, ref.bid_price);
                if (q) { push_sell_ref(q, ref.bid_price); return; }
            } else if (net < 0 && pos_ref < 0) {
                const Qty_t want_ref = static_cast<Qty_t>(
                    std::min<int64_t>(static_cast<int64_t>(want), -pos_ref));
                const Qty_t q = cap_buy(cfg.venue_ref, want_ref,
                                        ref.ask_qty, ref.ask_price);
                if (q) { push_buy_ref(q, ref.ask_price); return; }
            }
        }

        // 2) Exit a matched pair: A = short real / long ref, B = the opposite.
        if (pos_real < 0 && pos_ref > 0) {
            const int64_t matched = std::min(-pos_real, pos_ref);
            const double pnl_per_unit =
                (avg_real - static_cast<double>(real.ask_price)) +
                (static_cast<double>(ref.bid_price) - avg_ref);
            if (matched > 0 && (pnl_per_unit >= cfg.exit_profit ||
                                hold_ticks_ >= cfg.max_hold_ticks)) {
                Qty_t q = static_cast<Qty_t>(
                    std::min<int64_t>(static_cast<int64_t>(lot), matched));
                q = std::min<Qty_t>(
                    cap_buy(cfg.venue_real, q, real.ask_qty, real.ask_price),
                    cap_sell(cfg.venue_ref, q, ref.bid_qty, ref.bid_price));
                if (q) {
                    push_buy_real(q, real.ask_price);
                    push_sell_ref(q, ref.bid_price);
                }
            }
            return;
        }
        if (pos_real > 0 && pos_ref < 0) {
            const int64_t matched = std::min(pos_real, -pos_ref);
            const double pnl_per_unit =
                (static_cast<double>(real.bid_price) - avg_real) +
                (avg_ref - static_cast<double>(ref.ask_price));
            if (matched > 0 && (pnl_per_unit >= cfg.exit_profit ||
                                hold_ticks_ >= cfg.max_hold_ticks)) {
                Qty_t q = static_cast<Qty_t>(
                    std::min<int64_t>(static_cast<int64_t>(lot), matched));
                q = std::min<Qty_t>(
                    cap_sell(cfg.venue_real, q, real.bid_qty, real.bid_price),
                    cap_buy(cfg.venue_ref, q, ref.ask_qty, ref.ask_price));
                if (q) {
                    push_sell_real(q, real.bid_price);
                    push_buy_ref(q, ref.ask_price);
                }
            }
            return;
        }

        // 3) Only a flat book opens a new pair.
        if (pos_real != 0 || pos_ref != 0) return;

        // A: real bid above ref offer -> buy ref, sell real.
        if (static_cast<int64_t>(real.bid_price) -
                static_cast<int64_t>(ref.ask_price) >= edge_) {
            int64_t q = static_cast<int64_t>(lot);
            q = std::min<int64_t>(q, cfg.max_pos - pos_ref);
            q = std::min<int64_t>(q, cfg.max_pos + pos_real);
            const Qty_t want = static_cast<Qty_t>(std::max<int64_t>(0, q));
            const Qty_t capped = std::min<Qty_t>(
                cap_sell(cfg.venue_real, want, real.bid_qty, real.bid_price),
                cap_buy(cfg.venue_ref, want, ref.ask_qty, ref.ask_price));
            if (capped) {
                push_buy_ref(capped, ref.ask_price);
                push_sell_real(capped, real.bid_price);
            }
            return;
        }

        // B: real offer below ref bid -> buy real, sell ref.
        if (static_cast<int64_t>(ref.bid_price) -
                static_cast<int64_t>(real.ask_price) >= edge_) {
            int64_t q = static_cast<int64_t>(lot);
            q = std::min<int64_t>(q, cfg.max_pos - pos_real);
            q = std::min<int64_t>(q, cfg.max_pos + pos_ref);
            const Qty_t want = static_cast<Qty_t>(std::max<int64_t>(0, q));
            const Qty_t capped = std::min<Qty_t>(
                cap_buy(cfg.venue_real, want, real.ask_qty, real.ask_price),
                cap_sell(cfg.venue_ref, want, ref.bid_qty, ref.bid_price));
            if (capped) {
                push_sell_ref(capped, ref.bid_price);
                push_buy_real(capped, real.ask_price);
            }
        }
    }

private:
    int hold_ticks_ = 0;

    void cancel_open() {
        if (priv_in.open_orders.empty()) return;
        strat::cancel_resting(*this, cfg.sym, cfg.venue_real);
        strat::cancel_resting(*this, cfg.sym, cfg.venue_ref);
    }

    const AccountView& account_at(uint8_t venue) const {
        return priv_in.accounts[venue];
    }

    // Available base for a sell at `venue` (checker view; invalid/infinite = no cap).
    Qty_t base_capacity(uint8_t venue) const {
        const AccountView& acc = account_at(venue);
        if (!acc.valid || acc.base_infinite) return Qty_max;
        return static_cast<Qty_t>(
            acc.base > acc.base_locked ? acc.base - acc.base_locked : 0);
    }

    // Available cash for a buy at `venue` and price `px`.
    Qty_t cash_capacity(uint8_t venue, Price_t px) const {
        const AccountView& acc = account_at(venue);
        if (!acc.valid || acc.cash_infinite) return Qty_max;
        if (acc.cash <= acc.cash_locked) return 0;
        const uint64_t avail = acc.cash - acc.cash_locked;
        const uint64_t price = std::max<uint64_t>(1, px);
        return static_cast<Qty_t>(
            std::min<uint64_t>(Qty_max, avail / price));
    }

    // Remaining room under the checker's absolute position limit at `venue`.
    Qty_t position_headroom(uint8_t venue) const {
        const AccountView& acc = account_at(venue);
        if (!acc.valid || acc.limit_infinite || acc.base_infinite) return Qty_max;
        if (acc.position_limit <= acc.base) return 0;
        return static_cast<Qty_t>(acc.position_limit - acc.base);
    }

    Qty_t cap_visible(Qty_t q, QtySum_t visible) const {
        if (!cfg.use_visible_qty) return q;
        const uint64_t v = std::min<uint64_t>(visible, Qty_max);
        return static_cast<Qty_t>(std::min<uint64_t>(q, v));
    }

    Qty_t cap_sell(uint8_t venue, Qty_t q, QtySum_t visible,
                   Price_t /*px*/) const {
        q = cap_visible(q, visible);
        return std::min<Qty_t>(q, base_capacity(venue));
    }

    Qty_t cap_buy(uint8_t venue, Qty_t q, QtySum_t visible, Price_t px) const {
        q = cap_visible(q, visible);
        q = std::min<Qty_t>(q, cash_capacity(venue, px));
        return std::min<Qty_t>(q, position_headroom(venue));
    }

    void push_sell_real(Qty_t q, Price_t px) {
        strat::push_order(out_next, cfg.venue_real, cfg.sym, Side_t::Sell,
                          px, q, next_id++);
    }
    void push_buy_real(Qty_t q, Price_t px) {
        strat::push_order(out_next, cfg.venue_real, cfg.sym, Side_t::Buy,
                          px, q, next_id++);
    }
    void push_sell_ref(Qty_t q, Price_t px) {
        strat::push_order(out_next, cfg.venue_ref, cfg.sym, Side_t::Sell,
                          px, q, next_id++);
    }
    void push_buy_ref(Qty_t q, Price_t px) {
        strat::push_order(out_next, cfg.venue_ref, cfg.sym, Side_t::Buy,
                          px, q, next_id++);
    }
};

} // namespace av
