#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <vector>

#include "core/instructions.hpp"

namespace av {

// FIFO of already-allocated OP records waiting to be fed to a real venue's
// core. The public shape mirrors OoOobookCPP's InstructionsFromCSV so that a
// venue can drive its core with the same handshake:
//
//   ready = core.op_queue.ready_num();
//   ops   = source.issue(ready);      // <= ready, pops them
//   core.comb(ops.size(), ops);
struct AgentOrderSource {
    std::deque<OP> pending;

    bool   empty() const         { return pending.empty(); }
    size_t get_valid_num() const { return pending.size(); }

    void push(const OP& op) { pending.push_back(op); }
    void clear()            { pending.clear(); }

    std::vector<OP> issue(size_t ready) {
        const size_t n = std::min(ready, pending.size());
        std::vector<OP> out;
        out.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            out.push_back(pending.front());
            pending.pop_front();
        }
        return out;
    }
};

} // namespace av
