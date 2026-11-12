#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <vector>

#include "core/instructions.hpp"

namespace av {

// FIFO of checked OPcontexts waiting to be fed to a real venue's engine_top.
// Price / qty / side / reject reason are already filled when the context enters
// this queue; engine_top::comb allocates the internal op ids.
//
//   ready = engine.op_queue.ready_num();
//   ctxs  = source.issue(ready);      // <= ready, pops them
//   engine.comb(ctxs.size(), ctxs);
struct AgentOrderSource {
    std::deque<OPcontext> pending;

    bool   empty() const         { return pending.empty(); }
    size_t get_valid_num() const { return pending.size(); }

    void push(const OPcontext& ctx) { pending.push_back(ctx); }
    void clear()                    { pending.clear(); }

    std::vector<OPcontext> issue(size_t ready) {
        const size_t n = std::min(ready, pending.size());
        std::vector<OPcontext> out;
        out.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            out.push_back(pending.front());
            pending.pop_front();
        }
        return out;
    }
};

} // namespace av
