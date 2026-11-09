#pragma once

// AgentVenue shared types: the exchange<->agent wire records and the agent
// output records. Fixed-size where they are meant to model a register/wire;
// the framework-side containers (PrivateView) may use std containers because
// AgentVenue explicitly allows pragmatic code.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/types.hpp"
#include "core/instructions.hpp"

namespace av {

using AccountId     = uint64_t;
using ClientOrderId = uint64_t;

constexpr size_t MAX_VENUES    = 2;
constexpr size_t DEPTH         = 5;   // top-N levels per side in VenueView
constexpr size_t MAX_SIGNALS   = 8;
constexpr size_t MAX_ORDERS_PER_TICK = 4;

constexpr Price_t REF_ANCHOR_PRICE = 10000;
constexpr Price_t REF_HALF_SPREAD  = 1;

// Demo guard on a single order's quantity. The checker will enforce real
// risk limits later; this is only a sanity cap for the random traders.
constexpr Qty_t MAX_AGENT_QTY = 100;

// A very large synthetic liquidity offered by the reference market.
constexpr QtySum_t REF_LIQUIDITY = 1ull << 40;

// ---------------------------------------------------------------------------
// Market data published to agents (delayed by DelayLine)
// ---------------------------------------------------------------------------
struct LevelView {
    Price_t  price = 0;
    QtySum_t qty   = 0;
};

struct VenueView {
    Sym_t      sym        = 0;
    Price_t    bid_price  = 0;   // 0 == empty side
    QtySum_t   bid_qty    = 0;
    Price_t    ask_price  = 0;
    QtySum_t   ask_qty    = 0;
    Price_t    last_price = 0;   // 0 == no trade yet
    Qty_t      last_qty   = 0;
    uint64_t   trade_count  = 0;
    QtySum_t   total_volume = 0;
    std::array<LevelView, DEPTH> bids{};  // [0] best, descending
    std::array<LevelView, DEPTH> asks{};  // [0] best, ascending
};

struct MarketView {
    uint64_t   tick  = 0;
    bool       valid = false;
    std::array<VenueView, MAX_VENUES> venues{};
};

struct MarketStats {
    Price_t  last_price   = 0;
    Qty_t    last_qty     = 0;
    uint64_t trade_count  = 0;
    QtySum_t total_volume = 0;

    void apply_trade(Price_t price, Qty_t qty) {
        last_price = price;
        last_qty   = qty;
        ++trade_count;
        total_volume += qty;
    }
};

// ---------------------------------------------------------------------------
// Signals (computed one cycle ahead, delivered together with the MarketView)
// ---------------------------------------------------------------------------
struct SignalSet {
    uint64_t tick  = 0;
    bool     valid = false;
    std::array<Price_t, MAX_VENUES> mid{};
    std::array<bool,    MAX_VENUES> mid_valid{};
    std::array<Qty_t,   MAX_VENUES> spread{};
    std::array<int64_t, MAX_SIGNALS> custom{};
};

// ---------------------------------------------------------------------------
// Private view: everything the exchange knows about this agent's orders
// ---------------------------------------------------------------------------
enum class RejectReason : uint8_t {
    None = 0,
    InvalidPrice,
    InvalidQty,
    QtyTooLarge,
    InvalidContract,
    InvalidVenue,
    UnknownCancelTarget,
    LimitNotCrossingRef,
    InsufficientFunds,
    PositionLimit,
    PostOnlyWouldCross,
    SelfTradePrevented,
    RiskLimit,
    Other,
};

inline const char* reject_reason_name(RejectReason r) {
    switch (r) {
        case RejectReason::None:               return "none";
        case RejectReason::InvalidPrice:       return "invalid_price";
        case RejectReason::InvalidQty:         return "invalid_qty";
        case RejectReason::QtyTooLarge:        return "qty_too_large";
        case RejectReason::InvalidContract:    return "invalid_contract";
        case RejectReason::InvalidVenue:       return "invalid_venue";
        case RejectReason::UnknownCancelTarget:return "unknown_cancel_target";
        case RejectReason::LimitNotCrossingRef:return "limit_not_crossing_ref";
        case RejectReason::InsufficientFunds:  return "insufficient_funds";
        case RejectReason::PositionLimit:      return "position_limit";
        case RejectReason::PostOnlyWouldCross: return "post_only_would_cross";
        case RejectReason::SelfTradePrevented: return "self_trade_prevented";
        case RejectReason::RiskLimit:          return "risk_limit";
        case RejectReason::Other:              return "other";
    }
    return "other";
}

struct Fill {
    uint8_t       venue     = 0;
    ClientOrderId client_id = 0;
    Side_t        side      = Side_t::Buy;
    Price_t       price     = 0;
    Qty_t         qty       = 0;
    bool          maker     = false;
};

struct OrderUpdate {
    enum class Kind : uint8_t { Accepted, Cancelled, Rejected, Filled };

    Kind          kind      = Kind::Accepted;
    uint8_t       venue     = 0;
    ClientOrderId client_id = 0;
    RejectReason  reason    = RejectReason::None;
    Qty_t         remaining = 0;
};

struct OpenOrder {
    uint8_t       venue     = 0;
    ClientOrderId client_id = 0;
    Side_t        side      = Side_t::Buy;
    Price_t       price     = 0;
    Qty_t         remaining = 0;
};

struct PrivateView {
    uint64_t tick = 0;
    std::vector<Fill>        fills;
    std::vector<OrderUpdate> updates;
    std::vector<OpenOrder>   open_orders;  // current resting orders of this agent
};

// ---------------------------------------------------------------------------
// Portfolio (spot style, contract multiplier = 1)
// ---------------------------------------------------------------------------
struct VenuePortfolio {
    int64_t position      = 0;   // signed quantity
    int64_t cash          = 0;   // buy -> -, sell -> +
    double  avg_cost      = 0.0; // per unit
    double  realized_pnl  = 0.0;
    double  unrealized_pnl = 0.0; // filled by the engine using the delayed mark
};

struct PortfolioView {
    uint64_t tick = 0;
    std::array<VenuePortfolio, MAX_VENUES> venues{};
};

// ---------------------------------------------------------------------------
// Agent order output (wire written in comb, registered for the exchange)
// ---------------------------------------------------------------------------
struct AgentOrder {
    uint8_t       venue     = 0;
    OPKind        kind      = OPKind::Limit;
    Sym_t         sym       = 0;
    Side_t        side      = Side_t::Buy;
    Price_t       price     = 0;
    Qty_t         qty       = 0;
    ClientOrderId client_id = 0;  // new order id, or the target for a cancel
};

struct AgentOutput {
    std::array<AgentOrder, MAX_ORDERS_PER_TICK> orders{};
    size_t n = 0;

    void clear() { n = 0; }
    bool push(const AgentOrder& o) {
        if (n >= MAX_ORDERS_PER_TICK) return false;
        orders[n++] = o;
        return true;
    }
};

} // namespace av
