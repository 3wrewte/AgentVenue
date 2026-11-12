#include "doctest.h"

#include <unordered_map>

#include "agents/strategies/random_trader.hpp"
#include "core/phase.hpp"
#include "runner/fill_router.hpp"
#include "runner/venue.hpp"

using namespace av;

namespace {

OPcontext make_limit(AccountId account, ClientOrderId client, Side_t side,
                     Price_t price, Qty_t qty, Seq_t seq = 1) {
    OPcontext c{};
    c.kind           = OPKind::Limit;
    c.Seq            = seq;
    c.ContractId     = 1;
    c.AccountId      = account;
    c.ClientOrderId  = client;
    c.Side           = side;
    c.Price          = price;
    c.qty            = qty;
    c.reject_reason  = OPIssue::Default;
    c.reject_detail  = 0;
    return c;
}

OPcontext make_cancel(AccountId account, ClientOrderId target, Seq_t seq) {
    OPcontext c{};
    c.kind           = OPKind::Cancel;
    c.Seq            = seq;
    c.ContractId     = 1;
    c.AccountId      = account;
    c.ClientOrderId  = target;
    c.Side           = Side_t::Buy;
    c.Price          = 0;
    c.qty            = 0;
    c.reject_reason  = OPIssue::Default;
    c.reject_detail  = 0;
    return c;
}

std::unordered_map<AccountId_t, AccountState> account_with(
    AccountId account, std::optional<uint64_t> cash,
    std::optional<uint64_t> base = 0,
    std::optional<uint64_t> position_limit = std::nullopt) {
    std::unordered_map<AccountId_t, AccountState> init;
    AccountState st;
    st.cash           = cash;
    st.base           = base;
    st.position_limit = position_limit;
    init[account] = st;
    return init;
}

// The OP -> MOP -> UOP queues use registered heads/tails, so an OP submitted
// in one cycle produces its events one or two cycles later. Drain until the
// ROB is empty and collect everything (the SimEngine high-freq phase does the
// same for real).
VenueEvents run_until_done(RealVenue& real) {
    VenueEvents all;
    for (int i = 0; i < 16; ++i) {
        global_phase = Phase::Prepare;
        real.prepare();
        global_phase = Phase::Comb;
        VenueEvents ev = real.comb();
        global_phase = Phase::Commit;
        real.commit();

        all.trades.insert(all.trades.end(), ev.trades.begin(), ev.trades.end());
        all.ticks.insert(all.ticks.end(), ev.ticks.begin(), ev.ticks.end());
        all.replies.insert(all.replies.end(), ev.replies.begin(), ev.replies.end());

        if (real.source.empty() && real.op_pipeline_idle()) break;
    }
    return all;
}

} // namespace

TEST_CASE("checker rejects a buy that exceeds the cash balance") {
    RealVenue real;
    real.configure(0, 1, "", false);
    real.reset(account_with(7, 100));

    real.submit(make_limit(7, 1, Side_t::Buy, 100, 2));
    VenueEvents ev = run_until_done(real);

    REQUIRE(ev.replies.size() == 1);
    CHECK(ev.replies[0].kind == OPKind::NOOP);
    CHECK(ev.replies[0].reject_reason == OPIssue::InsufficientFunds);
    CHECK(ev.trades.empty());
    CHECK(real.checker.states[7].cash_locked == 0);
}

TEST_CASE("checker rejects a sell that exceeds the base balance") {
    RealVenue real;
    real.configure(0, 1, "", false);
    real.reset(account_with(7, 1'000'000, 1));

    real.submit(make_limit(7, 1, Side_t::Sell, 100, 2));
    VenueEvents ev = run_until_done(real);

    REQUIRE(ev.replies.size() == 1);
    CHECK(ev.replies[0].reject_reason == OPIssue::InsufficientBase);
    CHECK(real.checker.states[7].base_locked == 0);
}

TEST_CASE("checker rejects a buy that would exceed the position limit") {
    RealVenue real;
    real.configure(0, 1, "", false);
    real.reset(account_with(7, 1'000'000, 0, 5));

    real.submit(make_limit(7, 1, Side_t::Buy, 100, 6));
    VenueEvents ev = run_until_done(real);

    REQUIRE(ev.replies.size() == 1);
    CHECK(ev.replies[0].reject_reason == OPIssue::PositionLimit);
}

TEST_CASE("resting limit becomes an open order, cancel removes it and unlocks") {
    RealVenue real;
    real.configure(0, 1, "", false);
    real.reset(account_with(7, 10'000));

    RandomTrader agent(0, 7, 0, RandomTrader::Config{}, 123);
    agent.reset();

    FillRouter router;
    router.reset(1);
    router.set_accounts({&agent});

    real.submit(make_limit(7, 5, Side_t::Buy, 100, 3));
    router.track_submission(0, 7, 5, OPKind::Limit, Side_t::Buy, 100);
    VenueEvents ev = run_until_done(real);
    router.route_trades(0, ev.trades);
    router.route_replies(0, ev.replies);
    router.finalize_tick(0, {&agent}, MarketView{});

    REQUIRE(ev.replies.size() == 1);
    CHECK(ev.replies[0].kind == OPKind::Limit);
    CHECK(ev.replies[0].rested_qty == 3);
    CHECK(ev.replies[0].reject_reason == OPIssue::Default);
    CHECK(real.checker.states[7].cash_locked == 300);

    REQUIRE(agent.priv_in.open_orders.size() == 1);
    CHECK(agent.priv_in.open_orders[0].venue == 0);
    CHECK(agent.priv_in.open_orders[0].client_id == 5);
    CHECK(agent.priv_in.open_orders[0].side == Side_t::Buy);
    CHECK(agent.priv_in.open_orders[0].price == 100);
    CHECK(agent.priv_in.open_orders[0].remaining == 3);

    AccountView av = real.account_view(7);
    CHECK(av.valid);
    CHECK(!av.cash_infinite);
    CHECK(av.cash == 10'000);
    CHECK(av.cash_locked == 300);

    real.submit(make_cancel(7, 5, 2));
    VenueEvents cev = run_until_done(real);
    router.route_trades(0, cev.trades);
    router.route_replies(0, cev.replies);
    router.finalize_tick(1, {&agent}, MarketView{});

    REQUIRE(cev.replies.size() == 1);
    CHECK(cev.replies[0].kind == OPKind::Cancel);
    CHECK(cev.replies[0].cancel_found);
    CHECK(cev.replies[0].cancel_qty == 3);
    CHECK(real.checker.states[7].cash_locked == 0);
    CHECK(agent.priv_in.open_orders.empty());
    CHECK(real.checker.open.empty());
}

TEST_CASE("trades from two agents settle both accounts") {
    RealVenue real;
    real.configure(0, 1, "", false);
    std::unordered_map<AccountId_t, AccountState> init;
    {
        AccountState st;
        st.cash = 1'000'000;
        st.base = 100;
        init[7] = st;
    }
    {
        AccountState st;
        st.cash = 1'000'000;
        st.base = 100;
        init[8] = st;
    }
    real.reset(std::move(init));

    RandomTrader maker(0, 7, 0, RandomTrader::Config{}, 1);
    RandomTrader taker(1, 8, 0, RandomTrader::Config{}, 2);
    maker.reset();
    taker.reset();

    FillRouter router;
    router.reset(2);
    router.set_accounts({&maker, &taker});

    // Maker rests a sell at 100.
    real.submit(make_limit(7, 10, Side_t::Sell, 100, 4));
    router.track_submission(0, 7, 10, OPKind::Limit, Side_t::Sell, 100);
    VenueEvents ev1 = run_until_done(real);
    router.route_trades(0, ev1.trades);
    router.route_replies(0, ev1.replies);

    // Taker crosses it with a buy at 101.
    real.submit(make_limit(8, 20, Side_t::Buy, 101, 4, 2));
    router.track_submission(0, 8, 20, OPKind::Limit, Side_t::Buy, 101);
    VenueEvents ev2 = run_until_done(real);
    router.route_trades(0, ev2.trades);
    router.route_replies(0, ev2.replies);
    router.finalize_tick(0, {&maker, &taker}, MarketView{});

    REQUIRE(ev2.trades.size() == 1);
    CHECK(ev2.trades[0].price == 100);
    CHECK(ev2.trades[0].qty == 4);
    CHECK(ev2.trades[0].maker_cleared);

    CHECK(maker.portfolio_in.venues[0].position == -4);
    CHECK(maker.portfolio_in.venues[0].cash == 400);
    CHECK(taker.portfolio_in.venues[0].position == 4);
    CHECK(taker.portfolio_in.venues[0].cash == -400);

    CHECK(real.checker.states[7].cash == 1'000'400);
    CHECK(real.checker.states[7].base == 96);
    CHECK(real.checker.states[8].cash == 999'600);
    CHECK(real.checker.states[8].base == 104);
    CHECK(maker.priv_in.open_orders.empty());
    CHECK(taker.priv_in.open_orders.empty());
}
