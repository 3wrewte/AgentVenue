#include "doctest.h"

#include <cmath>

#include "runner/sim_engine.hpp"

using namespace av;

namespace {
void configure(SimEngine& e, uint64_t ticks, size_t agents, uint64_t seed) {
    e.cfg.record = false;
    e.cfg.ticks = ticks;
    e.cfg.noise_agents = agents;
    e.cfg.seed = seed;
    e.cfg.market_delay = 1;
    e.cfg.threads = 1;
    e.cfg.out_dir = "build/sim-test";
}
} // namespace

TEST_CASE("simulation is deterministic for a fixed seed") {
    SimEngine a;
    configure(a, 200, 20, 7);
    a.setup();
    a.run();

    SimEngine b;
    configure(b, 200, 20, 7);
    b.setup();
    b.run();

    CHECK(a.real.stats.trade_count == b.real.stats.trade_count);
    CHECK(a.real.stats.total_volume == b.real.stats.total_volume);
    REQUIRE(a.agents.size() == b.agents.size());
    for (size_t i = 0; i < a.agents.size(); ++i) {
        CHECK(a.agents[i]->portfolio_in.venues[0].position ==
              b.agents[i]->portfolio_in.venues[0].position);
        CHECK(a.agents[i]->portfolio_in.venues[0].cash ==
              b.agents[i]->portfolio_in.venues[0].cash);
    }
}

TEST_CASE("random traders generate trades") {
    SimEngine e;
    configure(e, 500, 40, 42);
    e.setup();
    e.run();

    CHECK(e.real.stats.trade_count > 0);
    CHECK(e.real.stats.total_volume > 0);
}

TEST_CASE("mm-arb scenario sets up roles, accounts and PnL classes") {
    SimEngine e;
    e.cfg.record  = false;
    e.cfg.ticks   = 100;
    e.cfg.seed    = 5;
    e.cfg.threads = 1;
    e.cfg.out_dir = "build/sim-test";
    e.cfg.scenario = Scenario::MmArb;

    MmArbConfig& m = e.cfg.mm_arb;
    m.n_mm    = 4;
    m.n_arb   = 3;
    m.n_noise = 10;
    m.cash    = 1'000'000;
    m.base    = 100;
    m.latency = 2;
    m.p1      = 2;
    m.p2      = 1;

    e.setup();

    REQUIRE(e.agents.size() == 17);
    CHECK(e.cfg.market_delay == 2);

    size_t n0 = 0, n1 = 0, n2 = 0;
    for (const AgentBase* a : e.agents) {
        if (a->class_id == 0) ++n0;
        else if (a->class_id == 1) ++n1;
        else if (a->class_id == 2) ++n2;
    }
    CHECK(n0 == 10);
    CHECK(n1 == 4);
    CHECK(n2 == 3);

    for (const AgentBase* a : e.agents) {
        if (a->class_id == 0) {
            CHECK_FALSE(a->account_cfg.cash.has_value());
            CHECK_FALSE(a->account_cfg.base.has_value());
        } else {
            REQUIRE(a->account_cfg.cash.has_value());
            REQUIRE(a->account_cfg.base.has_value());
            CHECK(*a->account_cfg.cash == 1'000'000);
            CHECK(*a->account_cfg.base == 100);
            REQUIRE(a->account_cfg.position_limit.has_value());
            CHECK(*a->account_cfg.position_limit == 200);
        }
    }

    CHECK(e.real.checker.states.size() == 17);

    e.run();

    const SimEngine::PnLSummary mm    = e.pnl_of_class(1);
    const SimEngine::PnLSummary arb   = e.pnl_of_class(2);
    const SimEngine::PnLSummary noise = e.pnl_of_class(0);
    CHECK(mm.n == 4);
    CHECK(arb.n == 3);
    CHECK(noise.n == 10);

    const double real_check =
        e.pnl_of_class(1, 0).total +
        e.pnl_of_class(2, 0).total +
        e.pnl_of_class(0, 0).total;
    CHECK(std::abs(real_check) < 1e-6);
}
