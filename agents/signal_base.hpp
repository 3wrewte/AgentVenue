#pragma once

#include "agents/agent_types.hpp"

namespace av {

// A signal module runs in the same prepare/comb/commit phases as the agents.
//
// It reads the MarketView that will be delivered to the agents *next* tick
// (delay.at(tick + 1 - market_delay)) and writes its fields into the shared
// SignalSet. Because the MarketView has a delay of at least one tick, that
// view is already available; the computed signal is registered for one cycle
// and delivered together with that same MarketView on the next tick.
class SignalModule {
public:
    virtual ~SignalModule() = default;

    virtual void reset()   {}
    virtual void prepare() {}
    virtual void comb(const MarketView& future_view, SignalSet& out) = 0;
    virtual void commit()  {}
};

} // namespace av
