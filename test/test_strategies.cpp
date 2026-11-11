#include "doctest.h"

#include <cstddef>
#include <cstdint>

#include "agents/agent_types.hpp"
#include "agents/strategies/arbitrage_strategy.hpp"
#include "agents/strategies/macd_strategy.hpp"
#include "agents/strategies/momentum_strategy.hpp"

using namespace av;

namespace {

// A two-venue view: real book at (bid, ask), the fixed synthetic reference
// market on venue 1 at REF_ANCHOR_PRICE +- REF_HALF_SPREAD.
MarketView make_view(uint64_t tick, int64_t real_bid, int64_t real_ask) {
    MarketView v;
    v.tick  = tick;
    v.valid = true;

    v.venues[0].sym       = 1;
    v.venues[0].bid_price = static_cast<Price_t>(real_bid);
    v.venues[0].ask_price = static_cast<Price_t>(real_ask);
    v.venues[0].bid_qty   = 100;
    v.venues[0].ask_qty   = 100;

    v.venues[1].sym       = 1;
    v.venues[1].bid_price = static_cast<Price_t>(REF_ANCHOR_PRICE - REF_HALF_SPREAD);
    v.venues[1].ask_price = static_cast<Price_t>(REF_ANCHOR_PRICE + REF_HALF_SPREAD);
    v.venues[1].bid_qty   = REF_LIQUIDITY;
    v.venues[1].ask_qty   = REF_LIQUIDITY;
    return v;
}

// One agent tick against no venue. `fill=true` immediately credits every
// non-cancel order to the position, which is what a taker at the touch gets.
void run_tick(AgentBase& a, const MarketView& v, bool fill) {
    a.prepare();
    a.mkt_in = v;
    a.comb();
    a.commit();

    if (!fill) return;
    for (size_t i = 0; i < a.out_committed.n; ++i) {
        const AgentOrder& o = a.out_committed.orders[i];
        if (o.kind == OPKind::Cancel) continue;
        const int64_t q = static_cast<int64_t>(o.qty);
        a.portfolio_in.venues[o.venue].position +=
            (o.side == Side_t::Buy) ? q : -q;
    }
}

// Every order must survive the framework-side checks in SimEngine::validate().
void check_wire(const AgentOrder& o) {
    CHECK(static_cast<size_t>(o.venue) < MAX_VENUES);
    if (o.kind == OPKind::Cancel) return;
    CHECK(o.qty >= 1);
    CHECK(o.qty <= MAX_AGENT_QTY);
    CHECK(o.price >= 1);
    CHECK(o.price <= Price_max);
}

} // namespace

TEST_CASE("macd: long in an uptrend, short after the reversal") {
    MacdStrategy::Config cfg;
    MacdStrategy a(1, 1'000'001ull, 1u, cfg);
    a.reset();

    int orders = 0;
    auto tick = [&](int t, int64_t bid) {
        run_tick(a, make_view(static_cast<uint64_t>(t), bid, bid + 2), true);
        for (size_t i = 0; i < a.out_committed.n; ++i) {
            check_wire(a.out_committed.orders[i]);
            if (a.out_committed.orders[i].kind != OPKind::Cancel) ++orders;
        }
    };

    for (int t = 0; t < 400; ++t) tick(t, 10000 + 2 * t);       // +2 ticks / bar

    CHECK(orders > 0);
    CHECK(a.target() == static_cast<int64_t>(cfg.lot));         // golden cross
    CHECK(a.portfolio_in.venues[0].position == static_cast<int64_t>(cfg.lot));

    const int64_t top = 10000 + 2 * 400;
    for (int t = 0; t < 200; ++t) tick(400 + t, top - 2 * t);   // reversal

    CHECK(a.target() == -static_cast<int64_t>(cfg.lot));        // death cross
    CHECK(a.portfolio_in.venues[0].position == -static_cast<int64_t>(cfg.lot));
}

TEST_CASE("momentum: long in an uptrend, flat once the trend dies") {
    MomentumStrategy::Config cfg;
    MomentumStrategy a(2, 1'000'002ull, 2u, cfg);
    a.reset();

    int64_t px = 10000;
    for (int t = 0; t < 120; ++t) {  // +1 tick per bar
        run_tick(a, make_view(static_cast<uint64_t>(t), px + 1, px + 3), true);
        ++px;
    }
    CHECK(a.target() == static_cast<int64_t>(cfg.lot));
    CHECK(a.portfolio_in.venues[0].position == static_cast<int64_t>(cfg.lot));

    for (int t = 0; t < 120; ++t) {  // flat market: the return decays to zero
        run_tick(a, make_view(static_cast<uint64_t>(1000 + t), px + 1, px + 3), true);
    }
    CHECK(a.target() == 0);
    CHECK(a.portfolio_in.venues[0].position == 0);
}

TEST_CASE("arbitrage: buys the ref offer when the real bid is above it") {
    ArbitrageStrategy::Config cfg;
    cfg.max_pos = 10;
    ArbitrageStrategy a(3, 1'000'003ull, 3u, cfg);
    a.reset();

    // real bid 10005 > ref ask 10001 -> buy ref, sell real.
    run_tick(a, make_view(0, 10005, 10007), false);

    REQUIRE(a.out_committed.n == 2);
    const AgentOrder& buy  = a.out_committed.orders[0];
    const AgentOrder& sell = a.out_committed.orders[1];

    CHECK(buy.venue  == cfg.venue_ref);
    CHECK(buy.side   == Side_t::Buy);
    CHECK(buy.price  == static_cast<Price_t>(REF_ANCHOR_PRICE + REF_HALF_SPREAD));

    CHECK(sell.venue == cfg.venue_real);
    CHECK(sell.side  == Side_t::Sell);
    CHECK(sell.price == static_cast<Price_t>(10005));
}

TEST_CASE("arbitrage: stands down when the venues do not cross") {
    ArbitrageStrategy a(4, 1'000'004ull, 3u, ArbitrageStrategy::Config{});
    a.reset();

    run_tick(a, make_view(0, 9998, 10002), false);
    CHECK(a.out_committed.n == 0);
}

TEST_CASE("exec: a stale resting order is cancelled, never stacked on") {
    MomentumStrategy::Config cfg;
    MomentumStrategy a(5, 1'000'005ull, 2u, cfg);
    a.reset();
    a.exec.max_rest_ticks = 2;

    // Pretend an earlier order of ours is still resting on the real venue.
    a.priv_in.open_orders.push_back(OpenOrder{0, 777, Side_t::Buy, 10000, 5});

    bool saw_cancel = false;
    int64_t px = 10000;
    for (int t = 0; t < 60 && !saw_cancel; ++t) {
        run_tick(a, make_view(static_cast<uint64_t>(t), px + 1, px + 3), false);
        for (size_t i = 0; i < a.out_committed.n; ++i) {
            const AgentOrder& o = a.out_committed.orders[i];
            CHECK(o.kind == OPKind::Cancel);  // no new order while one rests
            if (o.kind == OPKind::Cancel) {
                CHECK(o.client_id == 777);
                saw_cancel = true;
            }
        }
        if (saw_cancel) a.priv_in.open_orders.clear();
    }

    CHECK(saw_cancel);
}
