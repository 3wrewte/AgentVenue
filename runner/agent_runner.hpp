#pragma once

#include <algorithm>
#include <cstddef>
#include <thread>
#include <vector>

#include "agents/agent_base.hpp"
#include "agents/agent_types.hpp"
#include "agents/signal_base.hpp"

namespace av {

// Simple fork-join parallel_for. With threads <= 1 it runs sequentially.
// Agents/signal modules are independent, so a chunk split is safe; the join
// after each phase is the phase barrier. A persistent pool can replace this
// once the simulation is tuned.
template <typename Fn>
inline void parallel_for(size_t n, size_t threads, Fn&& fn) {
    if (threads <= 1 || n <= 1) {
        for (size_t i = 0; i < n; ++i) fn(i);
        return;
    }
    const size_t workers = std::min(threads, n);
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (size_t t = 0; t < workers; ++t) {
        const size_t begin = n * t / workers;
        const size_t end   = n * (t + 1) / workers;
        pool.emplace_back([&fn, begin, end] {
            for (size_t i = begin; i < end; ++i) fn(i);
        });
    }
    for (auto& th : pool) th.join();
}

class AgentRunner {
public:
    explicit AgentRunner(size_t threads = 1) : threads_(threads) {}

    void set_threads(size_t t) { threads_ = t; }
    size_t threads() const { return threads_; }

    void add(AgentBase* a)     { agents_.push_back(a); }
    void add_signal(SignalModule* s) { signals_.push_back(s); }

    const std::vector<AgentBase*>& agents() const { return agents_; }
    std::vector<AgentBase*>& agents() { return agents_; }

    void tick_prepare() {
        parallel_for(agents_.size(), threads_, [&](size_t i) { agents_[i]->prepare(); });
        parallel_for(signals_.size(), threads_, [&](size_t i) { signals_[i]->prepare(); });
    }

    void tick_comb(const MarketView& future_view, SignalSet& signals_next) {
        parallel_for(agents_.size(), threads_, [&](size_t i) { agents_[i]->comb(); });
        parallel_for(signals_.size(), threads_, [&](size_t i) {
            signals_[i]->comb(future_view, signals_next);
        });
    }

    void tick_commit() {
        parallel_for(agents_.size(), threads_, [&](size_t i) { agents_[i]->commit(); });
        parallel_for(signals_.size(), threads_, [&](size_t i) { signals_[i]->commit(); });
    }

private:
    size_t threads_ = 1;
    std::vector<AgentBase*> agents_;
    std::vector<SignalModule*> signals_;
};

} // namespace av
