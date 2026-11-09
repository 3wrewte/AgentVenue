#include "doctest.h"

#include "runner/sim_engine.hpp"

using namespace av;

namespace {
void configure(SimEngine& e, uint64_t ticks, size_t agents, uint64_t seed) {
    e.cfg.record = false;
    e.cfg.ticks = ticks;
    e.cfg.total_agents = agents;
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
