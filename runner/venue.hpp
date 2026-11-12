#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agents/agent_types.hpp"
#include "bookkeeping/checker.hpp"
#include "core/addr.hpp"
#include "core/events.hpp"
#include "core/idvol.hpp"
#include "core/instructions.hpp"
#include "io/csvrecorder.hpp"
#include "runner/order_source.hpp"

// engine_top lives behind a pointer so engine_top.hpp (whose comb() is defined
// non-inline in the core) is included by exactly one translation unit.
struct engine_top;

namespace av {

inline CanonicalAddr_t level_canonical(Sym_t sym, Side_t side, Price_t price) {
    return CanonicalAddrPack(CanonicalAddrStructured{
        .sym = sym, .side = side, .price = price, .pos = 0});
}

// Build a MarketView entry for one real core's current book.
inline VenueView build_venue_view(IdVolMap& idvol, const MarketStats& stats, Sym_t sym) {
    VenueView v;
    v.sym = sym;

    const auto bid = idvol.meta.best_bid();
    if (bid) {
        v.bid_price = *bid;
        v.bid_qty = idvol.level_sum_ram(level_canonical(sym, Side_t::Buy, *bid));
    }
    const auto ask = idvol.meta.best_ask();
    if (ask) {
        v.ask_price = *ask;
        v.ask_qty = idvol.level_sum_ram(level_canonical(sym, Side_t::Sell, *ask));
    }

    size_t i = 0;
    auto p = bid;
    while (p && i < DEPTH) {
        v.bids[i++] = LevelView{*p, idvol.level_sum_ram(level_canonical(sym, Side_t::Buy, *p))};
        p = idvol.meta.next_bid_below(*p, 0);
    }

    i = 0;
    p = ask;
    while (p && i < DEPTH) {
        v.asks[i++] = LevelView{*p, idvol.level_sum_ram(level_canonical(sym, Side_t::Sell, *p))};
        p = idvol.meta.next_ask_after(*p, Price_max);
    }

    v.last_price   = stats.last_price;
    v.last_qty     = stats.last_qty;
    v.trade_count  = stats.trade_count;
    v.total_volume = stats.total_volume;
    return v;
}

struct VenueEvents {
    std::vector<TradeEvent>      trades;
    std::vector<TickEvent>       ticks;
    std::vector<InputReplyEvent> replies;
};

// ---------------------------------------------------------------------------
// Real venue: one OoOobookCPP engine_top + its Checker + CSV sinks.
// ---------------------------------------------------------------------------
struct RealVenue {
    uint8_t  index = 0;
    Sym_t    sym   = 1;

    std::unique_ptr<engine_top> engine;
    Checker        checker;
    AgentOrderSource source;
    MarketStats    stats;
    uint64_t       next_seq = 1;

    std::unique_ptr<TradeStreamToCSV> trades;
    std::unique_ptr<TickStreamToCSV>  ticks;

    RealVenue();
    ~RealVenue();
    RealVenue(const RealVenue&) = delete;
    RealVenue& operator=(const RealVenue&) = delete;

    void configure(uint8_t idx, Sym_t s, const std::string& out_dir, bool record) {
        index = idx;
        sym = s;
        if (record) {
            trades = std::make_unique<TradeStreamToCSV>(
                out_dir + "/trades_v" + std::to_string(index) + ".csv");
            ticks = std::make_unique<TickStreamToCSV>(
                out_dir + "/ticks_v" + std::to_string(index) + ".csv");
        }
    }

    void reset(std::unordered_map<AccountId_t, AccountState> init_states = {});
    void prepare();
    VenueEvents comb();
    void commit();
    void finish();

    // Run the pre-trade gate and queue the (possibly NOOP) context. A NOOP
    // context goes through the engine too, so its reject reason comes back as
    // a regular InputReplyEvent.
    void submit(const OPcontext& ctx) {
        source.push(checker.check(ctx));
    }

    VenueView view() const;
    bool op_pipeline_idle() const;

    AccountView account_view(AccountId_t account) const {
        AccountView v;
        auto it = checker.states.find(account);
        if (it == checker.states.end()) return v;
        const AccountState& s = it->second;
        v.valid          = true;
        v.cash           = s.cash.value_or(0);
        v.cash_locked    = s.cash_locked;
        v.cash_infinite  = !s.cash.has_value();
        v.base           = s.base.value_or(0);
        v.base_locked    = s.base_locked;
        v.base_infinite  = !s.base.has_value();
        v.position_limit = s.position_limit.value_or(0);
        v.limit_infinite = !s.position_limit.has_value();
        return v;
    }
};

// ---------------------------------------------------------------------------
// Synthetic reference venue: constant bid/ask, infinite liquidity, fills the
// orders queued in the previous agent tick. No core.
// ---------------------------------------------------------------------------
struct SyntheticVenue {
    struct Pending {
        uint32_t   agent = 0;
        AgentOrder order;
    };

    uint8_t  index   = 1;
    Sym_t    sym     = 1;
    Price_t  ref_bid = REF_ANCHOR_PRICE - REF_HALF_SPREAD;
    Price_t  ref_ask = REF_ANCHOR_PRICE + REF_HALF_SPREAD;
    MarketStats stats;
    std::vector<Pending> pending;

    void configure(uint8_t idx, Sym_t s, Price_t bid, Price_t ask) {
        index = idx;
        sym = s;
        ref_bid = bid;
        ref_ask = ask;
    }

    void reset() {
        stats = MarketStats{};
        pending.clear();
    }

    VenueView view(uint64_t /*tick*/) const {
        VenueView v;
        v.sym = sym;
        v.bid_price = ref_bid;
        v.bid_qty = REF_LIQUIDITY;
        v.ask_price = ref_ask;
        v.ask_qty = REF_LIQUIDITY;
        v.last_price = stats.last_price;
        v.last_qty = stats.last_qty;
        v.trade_count = stats.trade_count;
        v.total_volume = stats.total_volume;
        v.bids[0] = LevelView{ref_bid, REF_LIQUIDITY};
        v.asks[0] = LevelView{ref_ask, REF_LIQUIDITY};
        return v;
    }

    // Fill every order queued on the previous agent tick.
    std::vector<RefFill> step() {
        std::vector<RefFill> out;
        out.reserve(pending.size());
        for (const Pending& p : pending) {
            const bool buy = (p.order.side == Side_t::Buy);
            const Price_t px = buy ? ref_ask : ref_bid;
            Fill f;
            f.venue = index;
            f.client_id = p.order.client_id;
            f.side = p.order.side;
            f.price = px;
            f.qty = p.order.qty;
            f.maker = false;
            out.push_back(RefFill{p.agent, f});
            stats.apply_trade(px, p.order.qty);
        }
        pending.clear();
        return out;
    }
};

} // namespace av
