#include "doctest.h"

#include "runner/delay_line.hpp"

using namespace av;

TEST_CASE("delay line reads published ticks and empty out-of-range") {
    DelayLine<int, 4> d;
    d.reset();

    CHECK(d.at(-1) == 0);
    CHECK(d.at(0) == 0);

    d.publish(0, 10);
    d.publish(1, 11);
    d.publish(2, 12);

    CHECK(d.at(0) == 10);
    CHECK(d.at(1) == 11);
    CHECK(d.at(2) == 12);
    CHECK(d.at(3) == 0);   // not published yet
}

TEST_CASE("delay line wraps and drops old entries") {
    DelayLine<int, 4> d;
    d.reset();
    for (int i = 0; i < 10; ++i) d.publish(static_cast<uint64_t>(i), i + 1);

    CHECK(d.at(9) == 10);
    CHECK(d.at(6) == 7);
    CHECK(d.at(5) == 0);   // evicted (window is 6..9)
}
