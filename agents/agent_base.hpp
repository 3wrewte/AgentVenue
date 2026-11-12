#pragma once

#include "agents/agent_types.hpp"
#include <cstddef>

namespace av {

// Base class for an RTL-style agent.
//
// Per tick the runner drives the input "wires" (mkt_in / signals_in /
// priv_in / portfolio_in), then runs the usual prepare -> comb -> commit
// phases. The agent writes out_next during comb; commit publishes it as
// out_committed, which the runner collects at the gearshift.
class AgentBase {
public:
    uint32_t      id       = 0;
    uint32_t      class_id = 0;
    AccountId     account  = 0;
    AccountConfig account_cfg;   // used by the real venue's Checker on reset

    // ---- inputs driven by the framework every tick ----
    MarketView    mkt_in;
    SignalSet     signals_in;
    PrivateView   priv_in;
    PortfolioView portfolio_in;

    // ---- output register ----
    AgentOutput out_next;
    AgentOutput out_committed;

    virtual ~AgentBase() = default;

    void reset() {
        out_next.clear();
        out_committed.clear();
        priv_in.fills.clear();
        priv_in.updates.clear();
        priv_in.open_orders.clear();
        on_reset();
    }

    void prepare() {
        out_next.clear();
        on_prepare();
    }

    virtual void comb() = 0;

    void commit() {
        out_committed = out_next;
        on_commit();
    }

protected:
    virtual void on_reset()   {}
    virtual void on_prepare() {}
    virtual void on_commit()  {}
};

} // namespace av
