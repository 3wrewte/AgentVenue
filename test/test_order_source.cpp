#include "doctest.h"

#include "runner/order_source.hpp"

using namespace av;

namespace {
OP make_op(int id) {
    OP o{};
    o.id = static_cast<OrderID_t>(id);
    o.kind = OPKind::NOOP;
    return o;
}
} // namespace

TEST_CASE("order source hands out at most ready and pops in FIFO order") {
    AgentOrderSource s;
    s.clear();
    s.push(make_op(1));
    s.push(make_op(2));
    s.push(make_op(3));

    CHECK(s.get_valid_num() == 3);

    auto a = s.issue(2);
    REQUIRE(a.size() == 2);
    CHECK(a[0].id == 1);
    CHECK(a[1].id == 2);
    CHECK(s.get_valid_num() == 1);

    auto b = s.issue(5);
    REQUIRE(b.size() == 1);
    CHECK(b[0].id == 3);
    CHECK(s.empty());
}

TEST_CASE("order source with ready == 0 hands out nothing") {
    AgentOrderSource s;
    s.clear();
    s.push(make_op(1));
    CHECK(s.issue(0).empty());
    CHECK(s.get_valid_num() == 1);
}
