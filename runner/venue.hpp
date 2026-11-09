#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "agents/agent_types.hpp"
#include "bookkeeping/IDallocator.hpp"
#include "core/addr.hpp"
#include "core/instructions.hpp"
#include "core/top.hpp"
#include "io/csvrecorder.hpp"
#include "runner/order_source.hpp"

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

// ---------------------------------------------------------------------------
// Real venue: one OoOobookCPP core + its bookkeeping + CSV sinks.
// ---------------------------------------------------------------------------
struct RealVenue {
    uint8_t  index = 0;
    Sym_t    sym   = 1;

    core_top       core;
    IDallocator    id_alloc;
    AgentOrderSource source;
    MarketStats    stats;
    uint64_t       next_seq = 1;

    std::unique_ptr<TradeStreamToCSV> trades;
    std::unique_ptr<TickStreamToCSV>  ticks;

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

    void reset() {
        core.reset();
        id_alloc.reset();
        source.clear();
        stats = MarketStats{};
        next_seq = 1;
        if (trades) trades->reset();
        if (ticks)  ticks->reset();
    }

    void prepare() {
        core.prepare();
        if (trades) trades->prepare();
        if (ticks)  ticks->prepare();
    }

    std::vector<CanonicalEvent> comb() {
        const size_t ready = core.op_queue.ready_num();
        auto ops = source.issue(ready);

        // Always run one core tick, even with zero new ops: the OP ROB may
        // still hold older ops that need to drain.
        auto [n, events] = core.comb(ops.size(), ops);
        (void)n;

        auto lookup = [&](OrderID_t id) { return id_alloc.get(id); };
        if (trades) trades->record(events.size(), events, lookup);
        if (ticks)  ticks->record(events.size(), events, lookup);

        for (const CanonicalEvent& ev : events) {
            if (ev.kind != UopKind::TakeLine) continue;
            const auto& take = std::get<TakeLineResult>(ev.payload);
            for (size_t s = 0; s < take.trade_vol_vec.size(); ++s) {
                const Qty_t vol = take.trade_vol_vec[s];
                if (vol == 0) continue;
                const auto maker = id_alloc.get(take.trade_id_vec[s]);
                stats.apply_trade(maker ? maker->Price : 0, vol);
            }
        }
        return events;
    }

    void commit() {
        core.commit();
        if (trades) trades->commit();
        if (ticks)  ticks->commit();
    }

    void finish() {
        if (trades) trades->finish();
        if (ticks)  ticks->finish();
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
