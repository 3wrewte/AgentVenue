#include "doctest.h"

#include "runner/order_source.hpp"

using namespace av;

namespace {
OPcontext make_ctx(int seq) {
    OPcontext c{};
    c.kind = OPKind::NOOP;
    c.Seq = static_cast<Seq_t>(seq);
    c.reject_reason = OPIssue::Default;
    return c;
}
} // namespace

TEST_CASE("order source hands out at most ready and pops in FIFO order") {
    AgentOrderSource s;
    s.clear();
    s.push(make_ctx(1));
    s.push(make_ctx(2));
    s.push(make_ctx(3));

    CHECK(s.get_valid_num() == 3);

    auto a = s.issue(2);
    REQUIRE(a.size() == 2);
    CHECK(a[0].Seq == 1);
    CHECK(a[1].Seq == 2);
    CHECK(s.get_valid_num() == 1);

    auto b = s.issue(5);
    REQUIRE(b.size() == 1);
    CHECK(b[0].Seq == 3);
    CHECK(s.empty());
}

TEST_CASE("order source with ready == 0 hands out nothing") {
    AgentOrderSource s;
    s.clear();
    s.push(make_ctx(1));
    CHECK(s.issue(0).empty());
    CHECK(s.get_valid_num() == 1);
}
