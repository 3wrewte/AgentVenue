#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace av {

// Fixed-capacity ring of published snapshots, indexed by logical tick.
//
//   publish(t, v)  stores v under tick t
//   at(k)          returns the snapshot published at tick k, or an empty T{}
//                  when k is outside the retained window
//
// The simulation only ever reads recent ticks (market_delay is small), so a
// ring of CAP entries is enough.
template <typename T, size_t CAP>
class DelayLine {
public:
    static_assert(CAP > 1, "DelayLine needs at least two entries");

    void reset() {
        for (auto& x : ring_) x = T{};
        newest_ = -1;
    }

    void publish(uint64_t tick, const T& value) {
        ring_[tick % CAP] = value;
        newest_ = static_cast<int64_t>(tick);
    }

    const T& at(int64_t tick) const {
        static const T empty{};
        if (tick < 0) return empty;
        if (tick > newest_) return empty;
        if (tick < newest_ - static_cast<int64_t>(CAP) + 1) return empty;
        return ring_[static_cast<uint64_t>(tick) % CAP];
    }

private:
    std::array<T, CAP> ring_{};
    int64_t newest_ = -1;
};

} // namespace av
