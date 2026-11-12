#pragma once

// MACD (12 / 26 / 9) on the delayed mid price; one agent tick == one bar.
//
//   DIF   = EMA(mid, fast) - EMA(mid, slow)
//   DEA   = EMA(DIF, signal)                 (the MACD signal line)
//   HIST  = DIF - DEA                        (histogram, 2*MACD in some texts)
//
// Signal (a cross with a dead band, i.e. hysteresis):
//   HIST > +min_hist and not already long  -> target +lot   ("golden cross")
//   HIST < -min_hist and not already short -> target -lot   ("death cross")
//
// Staying long while HIST only decays towards zero is intended: MACD is a trend
// follower, the position is kept until the opposite band is crossed. `min_hist`
// is in price ticks and is the only noise filter, so raising it is the first
// knob to turn in a noisy market.
//
// The position is executed with aggressive limits (`strat::PositionExec`), i.e.
// this is a taker strategy: the crossed spread is the cost of the signal.

#include <cstddef>
#include <cstdint>

#include "agents/agent_base.hpp"
#include "agents/agent_types.hpp"
#include "agents/strategies/strategy_exec.hpp"

namespace av {

class MacdStrategy : public AgentBase {
public:
    struct Config {
        int     fast     = 12;   // fast EMA period, in bars
        int     slow     = 26;   // slow EMA period, in bars
        int     signal   = 9;    // DEA period, in bars
        int     warmup   = 0;    // bars to wait before the first signal (0 = none)
        double  min_hist = 0.5;  // dead band on |HIST|, in price ticks
        Qty_t   lot      = 10;   // target position when long/short
        uint8_t venue    = strat::kRealVenue;
        Sym_t   sym      = 1;
    };

    Config cfg;
    strat::PositionExec exec;

    MacdStrategy() = default;

    MacdStrategy(uint32_t id_, AccountId account_, uint32_t class_id_, Config c)
        : cfg(c) {
        this->id       = id_;
        this->account  = account_;
        this->class_id = class_id_;
    }

    void on_reset() override {
        primed_   = false;
        ema_fast_ = 0.0;
        ema_slow_ = 0.0;
        dea_      = 0.0;
        hist_     = 0.0;
        bars_     = 0;
        target_   = 0;
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

        if (!primed_) {  // seed both EMAs with the first valid mid
            ema_fast_ = mid;
            ema_slow_ = mid;
            dea_      = 0.0;
            hist_     = 0.0;
            primed_   = true;
            bars_     = 0;
            return;
        }

        const int fast_n   = cfg.fast   > 0 ? cfg.fast   : 1;
        const int slow_n   = cfg.slow   > 0 ? cfg.slow   : 1;
        const int signal_n = cfg.signal > 0 ? cfg.signal : 1;
        const int warm_n   = cfg.warmup > 0 ? cfg.warmup : 0;

        const double af = 2.0 / (static_cast<double>(fast_n) + 1.0);
        const double as = 2.0 / (static_cast<double>(slow_n) + 1.0);
        const double ag = 2.0 / (static_cast<double>(signal_n) + 1.0);

        ema_fast_ += af * (mid - ema_fast_);
        ema_slow_ += as * (mid - ema_slow_);

        const double dif = ema_fast_ - ema_slow_;
        dea_ += ag * (dif - dea_);
        hist_ = dif - dea_;

        if (bars_ >= warm_n) {
            if (hist_ > cfg.min_hist && target_ <= 0) {
                target_ = static_cast<int64_t>(cfg.lot);
            } else if (hist_ < -cfg.min_hist && target_ >= 0) {
                target_ = -static_cast<int64_t>(cfg.lot);
            }
        }

        ++bars_;
        exec.move(*this, cfg.venue, cfg.sym, target_);
    }

    // Read-only accessors, handy in tests and when tuning.
    double  hist()   const { return hist_; }
    double  dif()    const { return ema_fast_ - ema_slow_; }
    double  dea()    const { return dea_; }
    int     bars()   const { return bars_; }
    int64_t target() const { return target_; }

private:
    bool    primed_   = false;
    double  ema_fast_ = 0.0;
    double  ema_slow_ = 0.0;
    double  dea_      = 0.0;
    double  hist_     = 0.0;
    int     bars_     = 0;
    int64_t target_   = 0;
};

} // namespace av
