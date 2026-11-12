#include "doctest.h"

#include <cstdint>

#include "agents/strategies/market_maker.hpp"

using namespace av;

namespace {

MarketView view(int64_t bid, int64_t ask) {
    MarketView v;
    v.tick  = 0;
    v.valid = true;
    v.venues[0].sym       = 1;
    v.venues[0].bid_price = static_cast<Price_t>(bid);
    v.venues[0].ask_price = static_cast<Price_t>(ask);
    v.venues[0].bid_qty   = 1000;
    v.venues[0].ask_qty   = 1000;
    return v;
}

void step(AgentBase& a, const MarketView& v) {
    a.prepare();
    a.mkt_in = v;
    a.comb();
    a.commit();
}

MarketMakerStrategy::Config config() {
    MarketMakerStrategy::Config c;
    c.half_spread    = 3;
    c.spread_sigma   = 0.0;
    c.lot            = 5;
    c.max_inventory  = 100;
    c.max_rest_ticks = 5;
    c.venue          = 0;
    c.sym            = 1;
    c.seed           = 1;
    return c;
}

} // namespace

TEST_CASE("mm quotes both sides around the delayed mid") {
    MarketMakerStrategy mm(0, 1, 1, config());
    mm.reset();

    step(mm, view(9999, 10001));

    REQUIRE(mm.out_committed.n == 2);
    const AgentOrder& bid = mm.out_committed.orders[0];
    const AgentOrder& ask = mm.out_committed.orders[1];

    CHECK(bid.kind == OPKind::Limit);
    CHECK(bid.side == Side_t::Buy);
    CHECK(bid.price == 9997);
    CHECK(bid.qty == 5);

    CHECK(ask.kind == OPKind::Limit);
    CHECK(ask.side == Side_t::Sell);
    CHECK(ask.price == 10003);
    CHECK(ask.qty == 5);

    CHECK(bid.price < ask.price);
}

TEST_CASE("mm stops quoting the side that would exceed inventory") {
    MarketMakerStrategy mm(0, 1, 1, config());
    mm.reset();

    mm.portfolio_in.venues[0].position = 100;
    step(mm, view(9999, 10001));
    REQUIRE(mm.out_committed.n == 1);
    CHECK(mm.out_committed.orders[0].side == Side_t::Sell);

    mm.portfolio_in.venues[0].position = -100;
    step(mm, view(9999, 10001));
    REQUIRE(mm.out_committed.n == 1);
    CHECK(mm.out_committed.orders[0].side == Side_t::Buy);
}

TEST_CASE("mm cancels a stale quote after max_rest_ticks") {
    MarketMakerStrategy mm(0, 1, 1, config());
    mm.reset();
    mm.priv_in.open_orders.push_back(
        OpenOrder{0, 7, Side_t::Buy, 9997, 5});

    bool saw_cancel = false;
    for (int t = 0; t < 20 && !saw_cancel; ++t) {
        step(mm, view(9999, 10001));
        if (mm.out_committed.n > 0) {
            REQUIRE(mm.out_committed.n == 1);
            const AgentOrder& o = mm.out_committed.orders[0];
            CHECK(o.kind == OPKind::Cancel);
            CHECK(o.client_id == 7);
            saw_cancel = true;
        }
    }
    CHECK(saw_cancel);
}

TEST_CASE("mm does not quote a one-sided book") {
    MarketMakerStrategy mm(0, 1, 1, config());
    mm.reset();

    step(mm, view(9999, 0));
    CHECK(mm.out_committed.n == 0);
}
