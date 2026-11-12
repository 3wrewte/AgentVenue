#pragma once

// Basic time-series momentum on the delayed mid price.
//
//   ret = mid(T) / mid(T - lookback) - 1
//
//   ret > +entry_bps   -> target +lot   (long)
//   ret < -entry_bps   -> target -lot   (short)
//   |ret| < exit_bps   -> target  0     (flat)
//   otherwise          -> hold the current target (hysteresis band)
//
// This is the textbook "buy what went up, sell what went down" rule, with a
// dead band so the position is not flipped by noise around zero.

#include <cmath>
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
        double  exit_bps  = 1.0;  // flatten when |ret| falls below this, in bps
        Qty_t   lot       = 10;
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

        const double entry = cfg.entry_bps * 1e-4;
        const double exit_ = cfg.exit_bps * 1e-4;

        if (r > entry) {
            target_ = static_cast<int64_t>(cfg.lot);
        } else if (r < -entry) {
            target_ = -static_cast<int64_t>(cfg.lot);
        } else if (std::fabs(r) < exit_) {
            target_ = 0;
        }

        exec.move(*this, cfg.venue, cfg.sym, target_);
    }

    double  ret()    const { return ret_; }
    int64_t target() const { return target_; }

private:
    std::vector<double> mids_;
    double  ret_    = 0.0;
    int64_t target_ = 0;
};

} // namespace av
