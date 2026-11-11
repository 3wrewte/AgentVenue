#pragma once

// Shared execution helper for the sample strategies.
//
// The framework has no IOC/FOK order type: a `Limit` that crosses the book is
// simply a taker (it trades at the maker's price), and whatever is left of it
// rests. Every strategy here is therefore "aggressive by construction": price
// at the opposite touch, and treat anything that still rests as work in
// progress (`priv_in.open_orders`).
//
// Units: `Price_t` is an integer tick price, 1 tick = 1 price unit.

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "agents/agent_base.hpp"
#include "agents/agent_types.hpp"

namespace av {
namespace strat {

// Venue indices as wired by SimEngine::setup().
constexpr uint8_t kRealVenue = 0;  // RealVenue (core_top)
constexpr uint8_t kRefVenue  = 1;  // SyntheticVenue (fixed bid/ask)

inline Price_t clamp_price(int64_t px) {
    if (px < 1) return static_cast<Price_t>(1);
    if (px > static_cast<int64_t>(Price_max)) return Price_max;
    return static_cast<Price_t>(px);
}

// Price that crosses the opposite side of `v` (i.e. is a taker).
//
// Falls back to last trade / the reference anchor when the book side is empty,
// in which case the order may rest instead of trade.
inline Price_t taker_price(const VenueView& v, Side_t side) {
    int64_t px;
    if (side == Side_t::Buy) {
        if (v.ask_price != 0)       px = static_cast<int64_t>(v.ask_price);
        else if (v.last_price != 0) px = static_cast<int64_t>(v.last_price) + 1;
        else                        px = static_cast<int64_t>(REF_ANCHOR_PRICE) + 1;
    } else {
        if (v.bid_price != 0)       px = static_cast<int64_t>(v.bid_price);
        else if (v.last_price != 0) px = static_cast<int64_t>(v.last_price) - 1;
        else                        px = static_cast<int64_t>(REF_ANCHOR_PRICE) - 1;
    }
    return clamp_price(px);
}

inline bool push_order(AgentOutput& out, uint8_t venue, Sym_t sym, Side_t side,
                       Price_t price, Qty_t qty, ClientOrderId client_id) {
    AgentOrder o{};
    o.venue     = venue;
    o.kind      = OPKind::Limit;
    o.sym       = sym;
    o.side      = side;
    o.price     = price;
    o.qty       = qty;
    o.client_id = client_id;
    return out.push(o);
}

// Cancel every resting order of this agent on the real venue. Cancels are only
// accepted there (the synthetic venue fills immediately and never rests an
// order). Returns the number of cancels pushed.
inline size_t cancel_resting(AgentBase& a, Sym_t sym, uint8_t real_venue = kRealVenue) {
    size_t n = 0;
    for (const OpenOrder& oo : a.priv_in.open_orders) {
        if (oo.venue != real_venue) continue;

        AgentOrder c{};
        c.venue     = oo.venue;
        c.kind      = OPKind::Cancel;
        c.sym       = sym;
        c.side      = oo.side;
        c.price     = 0;
        c.qty       = 0;
        c.client_id = oo.client_id;  // target of the cancel
        if (!a.out_next.push(c)) break;
        ++n;
    }
    return n;
}

// Keeps a position on one venue at a target level using aggressive limits.
//
// A target change is applied immediately (the old target is remembered). While
// an order is still resting, only `max_rest_ticks` ticks are spent waiting: an
// order that never trades gets cancelled and re-priced on the next tick, so the
// strategy neither churns every tick nor deadlocks behind a stale quote.
class PositionExec {
public:
    ClientOrderId next_id        = 1;  // unique within the agent's account
    int           max_rest_ticks = 3;  // ticks to wait for a resting order

    void reset() {
        next_id        = 1;
        target_        = 0;
        rest_ticks_    = 0;
    }

    // Returns the number of orders pushed this tick (0 = nothing to do,
    // waiting, or the output register is full).
    size_t move(AgentBase& a, uint8_t venue, Sym_t sym, int64_t target) {
        if (static_cast<size_t>(venue) >= MAX_VENUES) return 0;
        if (!a.mkt_in.valid) return 0;

        // Something of ours is still resting.
        if (!a.priv_in.open_orders.empty()) {
            const bool same_target = (target == target_);
            if (same_target && rest_ticks_++ < max_rest_ticks) return 0;
            rest_ticks_ = 0;
            return cancel_resting(a, sym);  // re-aim from a clean book next tick
        }
        rest_ticks_ = 0;
        target_ = target;

        const int64_t pos   = a.portfolio_in.venues[venue].position;
        const int64_t delta = target - pos;
        if (delta == 0) return 0;

        const Side_t  side = (delta > 0) ? Side_t::Buy : Side_t::Sell;
        const Price_t px   = taker_price(a.mkt_in.venues[venue], side);
        int64_t left = (delta > 0) ? delta : -delta;

        size_t sent = 0;
        while (left > 0) {
            const int64_t q =
                std::min<int64_t>(left, static_cast<int64_t>(MAX_AGENT_QTY));
            if (!push_order(a.out_next, venue, sym, side, px,
                            static_cast<Qty_t>(q), next_id)) {
                break;
            }
            ++next_id;
            left -= q;
            ++sent;
        }
        return sent;
    }

private:
    int64_t target_     = 0;
    int     rest_ticks_ = 0;
};

} // namespace strat
} // namespace av
