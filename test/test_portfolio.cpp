#include "doctest.h"

#include "runner/portfolio.hpp"

using namespace av;

TEST_CASE("portfolio long round trip realizes pnl") {
    PortfolioState p;
    PortfolioState::apply_fill(p.venues[0], Side_t::Buy, 100, 10);
    CHECK(p.venues[0].position == 10);
    CHECK(p.venues[0].cash == -1000);
    CHECK(p.venues[0].avg_cost == doctest::Approx(100.0));

    PortfolioState::apply_fill(p.venues[0], Side_t::Sell, 110, 10);
    CHECK(p.venues[0].position == 0);
    CHECK(p.venues[0].cash == 100);
    CHECK(p.venues[0].realized_pnl == doctest::Approx(100.0));
}

TEST_CASE("portfolio short round trip realizes pnl") {
    PortfolioState p;
    PortfolioState::apply_fill(p.venues[0], Side_t::Sell, 100, 5);
    CHECK(p.venues[0].position == -5);
    CHECK(p.venues[0].cash == 500);

    PortfolioState::apply_fill(p.venues[0], Side_t::Buy, 90, 5);
    CHECK(p.venues[0].position == 0);
    CHECK(p.venues[0].cash == 50);
    CHECK(p.venues[0].realized_pnl == doctest::Approx(50.0));
}

TEST_CASE("portfolio partial close keeps average cost") {
    PortfolioState p;
    PortfolioState::apply_fill(p.venues[0], Side_t::Buy, 100, 10);
    PortfolioState::apply_fill(p.venues[0], Side_t::Sell, 120, 4);

    CHECK(p.venues[0].position == 6);
    CHECK(p.venues[0].avg_cost == doctest::Approx(100.0));
    CHECK(p.venues[0].realized_pnl == doctest::Approx(80.0));
}
