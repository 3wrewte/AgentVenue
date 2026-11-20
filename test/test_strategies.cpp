#include "doctest.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "agents/agent_types.hpp"
#include "agents/signals/standard.hpp"
#include "agents/strategies/arbitrage_strategy.hpp"
#include "agents/strategies/macd_strategy.hpp"
#include "agents/strategies/momentum_strategy.hpp"
#include "agents/strategies/random_trader.hpp"

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

TEST_CASE("noise: side persistence makes consecutive sides repeat") {
    RandomTrader::Config cfg;
    cfg.lambda_new      = 50.0;
    cfg.lambda_cancel   = 0.0;
    cfg.p_buy           = 0.5;
    cfg.side_persistence = 1.0;
    cfg.qty_min         = 1;
    cfg.qty_max         = 1;
    cfg.limit_ratio     = 0.0;
    cfg.max_open        = 100;
    RandomTrader a(0, 1, 0, cfg, 123);

    run_tick(a, make_view(0, 10001, 10003), false);
    REQUIRE(a.out_committed.n > 1);

    const Side_t first = a.out_committed.orders[0].side;
    for (size_t i = 1; i < a.out_committed.n; ++i) {
        CHECK(a.out_committed.orders[i].side == first);
    }
}

TEST_CASE("signals: optional fundamental is fixed or exponential") {
    MarketView v;
    v.tick  = 100;
    v.valid = true;
    v.venues[0].bid_price = 10001;
    v.venues[0].ask_price = 10003;

    SignalSet out;

    StandardSignals off;
    off.comb(v, out);
    CHECK_FALSE(out.fundamental_valid[0]);

    StandardSignals fixed(true, 10000.0, 0.0);
    fixed.comb(v, out);
    CHECK(out.fundamental_valid[0]);
    CHECK(out.fundamental[0] == static_cast<Price_t>(10000));

    v.tick = 200;
    StandardSignals exp(true, 10000.0, 1e-4);
    exp.comb(v, out);
    const auto expected = static_cast<Price_t>(
        std::llround(10000.0 * std::exp(0.02)));
    CHECK(out.fundamental[0] == expected);

    StandardSignals jumps(true, 10000.0, 0.0, 100, 0.05, 1);
    v.tick = 99;
    jumps.comb(v, out);
    CHECK(out.fundamental[0] == static_cast<Price_t>(10000));
    v.tick = 100;
    jumps.comb(v, out);
    CHECK(out.fundamental[0] != static_cast<Price_t>(10000));

    v.tick = 100;
    StandardSignals per_venue(true, 10000.0, 0.0, 100, 0.05, 1, true);
    per_venue.comb(v, out);
    CHECK(out.fundamental_valid[0]);
    CHECK(out.fundamental_valid[1]);
    CHECK(out.fundamental[0] != out.fundamental[1]);
}

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

TEST_CASE("momentum: builds a long in lot/4 steps") {
    MomentumStrategy::Config cfg;
    MomentumStrategy a(6, 1'000'006ull, 2u, cfg);
    a.reset();

    const int64_t step = static_cast<int64_t>(cfg.lot) / 4;
    REQUIRE(step > 1);

    int64_t px = 10000;
    for (int t = 0; t <= 20; ++t) {  // warm-up, then the first signal
        run_tick(a, make_view(static_cast<uint64_t>(t), px + 1, px + 3), true);
        ++px;
    }
    CHECK(a.target() == step);
    CHECK(a.portfolio_in.venues[0].position == step);

    for (int t = 21; t < 30; ++t) {
        run_tick(a, make_view(static_cast<uint64_t>(t), px + 1, px + 3), true);
        ++px;
    }
    CHECK(a.target() == static_cast<int64_t>(cfg.lot));
    CHECK(a.portfolio_in.venues[0].position == static_cast<int64_t>(cfg.lot));
}

TEST_CASE("momentum: clears a long at zero, then may build a short") {
    MomentumStrategy::Config cfg;
    MomentumStrategy a(7, 1'000'007ull, 2u, cfg);
    a.reset();

    int64_t px = 10000;
    for (int t = 0; t < 120; ++t) {
        run_tick(a, make_view(static_cast<uint64_t>(t), px + 1, px + 3), true);
        ++px;
    }
    REQUIRE(a.target() == static_cast<int64_t>(cfg.lot));
    REQUIRE(a.portfolio_in.venues[0].position == static_cast<int64_t>(cfg.lot));

    bool flat_after_long = false;
    bool short_seen = false;
    for (int t = 0; t < 200; ++t) {
        run_tick(a, make_view(static_cast<uint64_t>(1000 + t), px + 1, px + 3), true);
        --px;
        if (a.target() == 0 && a.portfolio_in.venues[0].position == 0) {
            flat_after_long = true;
        }
        if (a.target() < 0) {
            CHECK(flat_after_long);
            short_seen = true;
        }
    }

    CHECK(flat_after_long);
    CHECK(short_seen);
    CHECK(a.target() == -static_cast<int64_t>(cfg.lot));
    CHECK(a.portfolio_in.venues[0].position == -static_cast<int64_t>(cfg.lot));
}

TEST_CASE("momentum: contrarian sign fades an uptrend") {
    MomentumStrategy::Config cfg;
    cfg.signal_sign = -1;
    MomentumStrategy a(8, 1'000'008ull, 2u, cfg);
    a.reset();

    int64_t px = 10000;
    for (int t = 0; t < 120; ++t) {
        run_tick(a, make_view(static_cast<uint64_t>(t), px + 1, px + 3), true);
        ++px;
    }
    CHECK(a.target() == -static_cast<int64_t>(cfg.lot));
    CHECK(a.portfolio_in.venues[0].position == -static_cast<int64_t>(cfg.lot));
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

TEST_CASE("arbitrage: net-edge requires the exit half-spread cost") {
    ArbitrageStrategy::Config cfg;
    cfg.max_pos = 100;
    cfg.edge    = 3;
    cfg.net_edge = true;
    ArbitrageStrategy a(11, 1'000'011ull, 2u, cfg);
    a.reset();

    // Real spread 2, ref spread 2 -> exit half-spread 2; cross 4 -> net 2.
    run_tick(a, make_view(0, 10005, 10007), false);
    CHECK(a.out_committed.n == 0);

    cfg.edge = 2;
    ArbitrageStrategy b(12, 1'000'012ull, 2u, cfg);
    b.reset();
    run_tick(b, make_view(0, 10005, 10007), false);
    REQUIRE(b.out_committed.n == 2);
}

TEST_CASE("arbitrage: settles a matched pair for cross-venue delivery") {
    ArbitrageStrategy::Config cfg;
    cfg.max_pos = 100;
    cfg.settle_after_pair = true;
    ArbitrageStrategy a(13, 1'000'013ull, 2u, cfg);
    a.reset();

    a.portfolio_in.venues[cfg.venue_real].position = 10;
    a.portfolio_in.venues[cfg.venue_real].avg_cost = 10000.0;
    a.portfolio_in.venues[cfg.venue_ref].position = -10;
    a.portfolio_in.venues[cfg.venue_ref].avg_cost = 10001.0;

    run_tick(a, make_view(0, 10000, 10002), false);

    REQUIRE(a.out_committed.settlement.active);
    CHECK(a.out_committed.settlement.long_venue == cfg.venue_real);
    CHECK(a.out_committed.settlement.short_venue == cfg.venue_ref);
    CHECK(a.out_committed.settlement.qty == 10);
    CHECK(a.out_committed.n == 0);
}

TEST_CASE("arbitrage: exits a profitable matched pair") {
    ArbitrageStrategy::Config cfg;
    cfg.max_pos = 100;
    ArbitrageStrategy a(6, 1'000'006ull, 2u, cfg);
    a.reset();

    // A-created position: short real @ 10005, long ref @ 10001.
    a.portfolio_in.venues[cfg.venue_real].position = -10;
    a.portfolio_in.venues[cfg.venue_real].avg_cost = 10005.0;
    a.portfolio_in.venues[cfg.venue_ref].position = 10;
    a.portfolio_in.venues[cfg.venue_ref].avg_cost = 10001.0;

    // Exit B: buy real @ 10000, sell ref @ 9999; pnl/unit = 5 - 2 = 3.
    run_tick(a, make_view(0, 9998, 10000), false);

    REQUIRE(a.out_committed.n == 2);
    CHECK(a.out_committed.orders[0].venue == cfg.venue_real);
    CHECK(a.out_committed.orders[0].side == Side_t::Buy);
    CHECK(a.out_committed.orders[0].price == 10000);
    CHECK(a.out_committed.orders[0].qty == 10);

    CHECK(a.out_committed.orders[1].venue == cfg.venue_ref);
    CHECK(a.out_committed.orders[1].side == Side_t::Sell);
    CHECK(a.out_committed.orders[1].price == 9999);
    CHECK(a.out_committed.orders[1].qty == 10);
}

TEST_CASE("arbitrage: forces an exit after max_hold_ticks") {
    ArbitrageStrategy::Config cfg;
    cfg.max_pos = 100;
    cfg.max_hold_ticks = 1;
    ArbitrageStrategy a(7, 1'000'007ull, 2u, cfg);
    a.reset();

    a.portfolio_in.venues[cfg.venue_real].position = -10;
    a.portfolio_in.venues[cfg.venue_real].avg_cost = 10005.0;
    a.portfolio_in.venues[cfg.venue_ref].position = 10;
    a.portfolio_in.venues[cfg.venue_ref].avg_cost = 10001.0;

    // The exit is bad on average, but the hold timer forces it.
    run_tick(a, make_view(0, 10008, 10010), false);

    REQUIRE(a.out_committed.n == 2);
    CHECK(a.out_committed.orders[0].side == Side_t::Buy);
    CHECK(a.out_committed.orders[0].price == 10010);
    CHECK(a.out_committed.orders[1].side == Side_t::Sell);
    CHECK(a.out_committed.orders[1].price == 9999);
}

TEST_CASE("arbitrage: sizes an entry by the visible real quantity") {
    ArbitrageStrategy::Config cfg;
    cfg.max_pos = 100;
    ArbitrageStrategy a(8, 1'000'008ull, 2u, cfg);
    a.reset();

    MarketView v = make_view(0, 10005, 10007);  // A: 10005 - 10001 = 4
    v.venues[0].bid_qty = 3;
    run_tick(a, v, false);

    REQUIRE(a.out_committed.n == 2);
    CHECK(a.out_committed.orders[0].side == Side_t::Buy);   // ref
    CHECK(a.out_committed.orders[0].qty == 3);
    CHECK(a.out_committed.orders[1].side == Side_t::Sell);  // real
    CHECK(a.out_committed.orders[1].qty == 3);
}

TEST_CASE("arbitrage: respects the checker's available base") {
    ArbitrageStrategy::Config cfg;
    cfg.max_pos = 100;
    ArbitrageStrategy a(9, 1'000'009ull, 2u, cfg);
    a.reset();

    a.priv_in.accounts[cfg.venue_real].valid = true;
    a.priv_in.accounts[cfg.venue_real].cash_infinite = true;
    a.priv_in.accounts[cfg.venue_real].base_infinite = false;
    a.priv_in.accounts[cfg.venue_real].base = 2;
    a.priv_in.accounts[cfg.venue_real].base_locked = 0;
    a.priv_in.accounts[cfg.venue_real].limit_infinite = true;

    run_tick(a, make_view(0, 10005, 10007), false);

    REQUIRE(a.out_committed.n == 2);
    CHECK(a.out_committed.orders[0].side == Side_t::Buy);   // ref
    CHECK(a.out_committed.orders[0].qty == 2);
    CHECK(a.out_committed.orders[1].side == Side_t::Sell);  // real
    CHECK(a.out_committed.orders[1].qty == 2);
}

TEST_CASE("arbitrage: flattens an unmatched net exposure") {
    ArbitrageStrategy::Config cfg;
    cfg.max_pos = 100;
    ArbitrageStrategy a(10, 1'000'010ull, 2u, cfg);
    a.reset();

    a.portfolio_in.venues[cfg.venue_real].position = 10;
    a.portfolio_in.venues[cfg.venue_real].avg_cost = 10000.0;

    run_tick(a, make_view(0, 10005, 10007), false);

    REQUIRE(a.out_committed.n == 1);
    CHECK(a.out_committed.orders[0].venue == cfg.venue_real);
    CHECK(a.out_committed.orders[0].side == Side_t::Sell);
    CHECK(a.out_committed.orders[0].price == 10005);
    CHECK(a.out_committed.orders[0].qty == 10);
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
