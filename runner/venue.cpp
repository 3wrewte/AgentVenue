#include "runner/venue.hpp"

#include <utility>

#include "core/op.hpp"
#include "engine_top.hpp"

namespace av {

RealVenue::RealVenue() : engine(std::make_unique<engine_top>()) {}
RealVenue::~RealVenue() = default;

void RealVenue::reset(std::unordered_map<AccountId_t, AccountState> init_states) {
    engine->reset();
    checker.reset(init_states, 0.0);
    checker.open.clear();
    source.clear();
    stats = MarketStats{};
    next_seq = 1;
    if (trades) trades->reset();
    if (ticks)  ticks->reset();
}

void RealVenue::prepare() {
    engine->prepare();
    if (trades) trades->prepare();
    if (ticks)  ticks->prepare();
}

VenueEvents RealVenue::comb() {
    const size_t ready = engine->op_queue.ready_num();
    auto ctxs = source.issue(ready);

    // Always run one engine tick, even with zero new ops: the OP ROB may still
    // hold older ops that need to drain.
    auto result = engine->comb(ctxs.size(), ctxs);
    auto& [trade_events, tick_events, reply_events] = result;

    if (trades) trades->record(trade_events);
    if (ticks)  ticks->record(tick_events);

    if (!tick_events.empty()) {
        const TickEvent& tk = tick_events.back();
        stats.last_price   = tk.last_price;
        stats.last_qty     = tk.last_qty;
        stats.trade_count  = tk.trade_count;
        stats.total_volume = tk.total_volume;
    }

    checker.update(trade_events, reply_events);

    return VenueEvents{std::move(trade_events),
                       std::move(tick_events),
                       std::move(reply_events)};
}

void RealVenue::commit() {
    engine->commit();
    if (trades) trades->commit();
    if (ticks)  ticks->commit();
}

void RealVenue::finish() {
    if (trades) trades->finish();
    if (ticks)  ticks->finish();
}

VenueView RealVenue::view() const {
    return build_venue_view(engine->idvol, stats, sym);
}

bool RealVenue::op_pipeline_idle() const {
    return engine->op_queue.ready_num() == OP_ROB_LENGTH - 1;
}

} // namespace av
