#pragma once

// Basic time-series momentum on the delayed mid price.
//
//   ret = mid(T) / mid(T - lookback) - 1
//
// Entry (only from flat, after any pending exit has completed):
//   ret > +entry_bps   -> build a long target in lot/4 steps
//   ret < -entry_bps   -> build a short target in lot/4 steps
//
// Exit / reverse:
//   long  and ret <= exit_bps -> target 0 immediately
//   short and ret >= -exit_bps -> target 0 immediately
// Once the position is actually flat, a still-strong reverse signal
// (ret < -entry_bps / ret > +entry_bps) starts the opposite ramp.
//
// Exits are deliberately not rate-limited: clearing a wrong position is more
// important than getting a better average price.  Entries are rate-limited so
// a noisy signal does not put the whole lot on in one tick.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "agents/agent_base.hpp"
#include "agents/agent_types.hpp"
#include "agents/strategies/strategy_exec.hpp"

namespace av {

class MomentumStrategy : public AgentBase {
public:
    struct Config {
        size_t  lookback  = 20;   // bars of history for the return
        double  entry_bps = 3.0;  // enter when |ret| exceeds this, in bps
        double  exit_bps  = 0.0;  // close when ret crosses this (0 = at zero)
        Qty_t   lot       = 10;
        Qty_t   entry_step = 0;   // per-tick add size; 0 => max(1, lot/4)
        int8_t  signal_sign = 1;  // +1 momentum, -1 contrarian/fade
        uint8_t venue     = strat::kRealVenue;
        Sym_t   sym       = 1;
    };

    Config cfg;
    strat::PositionExec exec;

    MomentumStrategy() = default;

    MomentumStrategy(uint32_t id_, AccountId account_, uint32_t class_id_, Config c)
        : cfg(c) {
        this->id       = id_;
        this->account  = account_;
        this->class_id = class_id_;
    }

    void on_reset() override {
        mids_.clear();
        ret_    = 0.0;
        side_   = 0;
        target_ = 0;
        exec.reset();
    }

    void comb() override {
        if (static_cast<size_t>(cfg.venue) >= MAX_VENUES) return;
        if (!mkt_in.valid || mkt_in.venues[cfg.venue].bid_price == 0 ||
            mkt_in.venues[cfg.venue].ask_price == 0) {
            // No usable two-sided book this tick: do not leave a stale order
            // resting behind a quote we can no longer see.
            if (!priv_in.open_orders.empty()) {
                strat::cancel_resting(*this, cfg.sym, cfg.venue);
            }
            return;
        }

        const VenueView& v = mkt_in.venues[cfg.venue];

        const double mid = 0.5 * (static_cast<double>(v.bid_price) +
                                  static_cast<double>(v.ask_price));

        mids_.push_back(mid);
        const size_t keep = cfg.lookback + 1;
        while (mids_.size() > keep) mids_.erase(mids_.begin());
        if (mids_.size() < keep) return;  // still warming up

        const double old = mids_.front();
        if (old <= 0.0) return;
        const double r = mid / old - 1.0;
        ret_ = r;
        const double rs = r * static_cast<double>(cfg.signal_sign);

        const double entry = cfg.entry_bps * 1e-4;
        const double exit_ = cfg.exit_bps * 1e-4;
        const int64_t lot  = static_cast<int64_t>(cfg.lot);
        const int64_t step = entry_step();

        // A long is closed as soon as the return reaches the exit level; a
        // short is closed symmetrically.  Strong reverses are handled below,
        // but only after the position has actually become flat.
        if (side_ > 0 && rs <= exit_) {
            side_   = 0;
            target_ = 0;
        } else if (side_ < 0 && rs >= -exit_) {
            side_   = 0;
            target_ = 0;
        }

        const int64_t pos = portfolio_in.venues[cfg.venue].position;
        const bool flat = (pos == 0 && target_ == 0);

        if (side_ == 0) {
            if (flat && rs > entry) {
                side_   = 1;
                target_ = std::min(lot, step);
            } else if (flat && rs < -entry) {
                side_   = -1;
                target_ = std::max(-lot, -step);
            }
        } else if (side_ > 0) {
            // Add only while the current order has cleared; otherwise let
            // PositionExec finish/cancel it before moving the target again.
            if (priv_in.open_orders.empty() && target_ < lot) {
                target_ = std::min(lot, target_ + step);
            }
        } else {
            if (priv_in.open_orders.empty() && target_ > -lot) {
                target_ = std::max(-lot, target_ - step);
            }
        }

        exec.move(*this, cfg.venue, cfg.sym, target_);
    }

    double  ret()    const { return ret_; }
    int64_t target() const { return target_; }

private:
    int64_t entry_step() const {
        if (cfg.entry_step > 0) return static_cast<int64_t>(cfg.entry_step);
        return std::max<int64_t>(1, static_cast<int64_t>(cfg.lot) / 4);
    }

    std::vector<double> mids_;
    double  ret_    = 0.0;
    int     side_   = 0;   // +1 long, -1 short, 0 flat
    int64_t target_ = 0;   // execution target (ramped on entry)
};

} // namespace av
