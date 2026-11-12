#pragma once

#include <cstdint>
#include <cstddef>
#include <map>
#include <tuple>
#include <vector>

#include "agents/agent_base.hpp"
#include "agents/agent_types.hpp"
#include "core/events.hpp"
#include "core/instructions.hpp"
#include "runner/portfolio.hpp"

namespace av {

inline RejectReason reject_reason_from_issue(OPIssue issue) {
    switch (issue) {
        case OPIssue::Default:             return RejectReason::None;
        case OPIssue::InvalidPrice:        return RejectReason::InvalidPrice;
        case OPIssue::InvalidQty:          return RejectReason::InvalidQty;
        case OPIssue::InvalidContract:     return RejectReason::InvalidContract;
        case OPIssue::UnknownCancelTarget: return RejectReason::UnknownCancelTarget;
        case OPIssue::InsufficientFunds:   return RejectReason::InsufficientFunds;
        case OPIssue::PositionLimit:       return RejectReason::PositionLimit;
        case OPIssue::RiskLimit:           return RejectReason::RiskLimit;
        case OPIssue::InvalidAccount:      return RejectReason::InvalidAccount;
        case OPIssue::InsufficientBase:    return RejectReason::InsufficientBase;
        case OPIssue::RepeatedID:          return RejectReason::RepeatedID;
        case OPIssue::UnknownError:        return RejectReason::Other;
    }
    return RejectReason::Other;
}

// Turns the core's self-contained TradeEvent / InputReplyEvent streams into
// per-agent fills, order updates and open-order state, and applies fills to
// the spot portfolios.
//
// Open orders are keyed by (venue, account, client_id): the core's internal
// ids are private to engine_top now. The reply does not carry side/price, so
// the framework records every submitted limit in `pending_` at the gearshift
// and consumes it when the reply arrives.
class FillRouter {
public:
    void reset(size_t n_agents) {
        n_agents_ = n_agents;
        pending_fills_.assign(n_agents, {});
        pending_updates_.assign(n_agents, {});
        portfolios_.assign(n_agents, PortfolioState{});
        open_orders_.clear();
        pending_.clear();
        account_to_agent_.clear();
    }

    void set_accounts(const std::vector<AgentBase*>& agents) {
        account_to_agent_.clear();
        for (size_t i = 0; i < agents.size(); ++i) {
            account_to_agent_[agents[i]->account] = static_cast<uint32_t>(i);
        }
    }

    // Remember the wire fields needed to build an OpenOrder from a reply.
    void track_submission(uint8_t venue, AccountId account, ClientOrderId client_id,
                          OPKind kind, Side_t side, Price_t price) {
        pending_[OrderKey{venue, account, client_id}] = PendingEntry{kind, side, price};
    }

    void route_trades(uint8_t venue, const std::vector<TradeEvent>& trades);
    void route_replies(uint8_t venue, const std::vector<InputReplyEvent>& replies);

    void route_ref(uint8_t venue, const std::vector<RefFill>& fills);

    // Publish this tick's routing results and portfolio snapshot to each agent.
    void finalize_tick(uint64_t tick, const std::vector<AgentBase*>& agents,
                       const MarketView& agent_view);

    const PortfolioState& portfolio(uint32_t agent) const { return portfolios_[agent]; }
    size_t open_order_count() const { return open_orders_.size(); }

    // Queue an immediate update (used for framework-side rejects).
    void push_update(uint32_t agent, const OrderUpdate& u) {
        if (agent < pending_updates_.size()) pending_updates_[agent].push_back(u);
    }

private:
    struct OrderKey {
        uint8_t       venue   = 0;
        AccountId     account = 0;
        ClientOrderId client  = 0;

        bool operator<(const OrderKey& o) const {
            return std::tie(venue, account, client) <
                   std::tie(o.venue, o.account, o.client);
        }
    };

    struct PendingEntry {
        OPKind  kind  = OPKind::Limit;
        Side_t  side  = Side_t::Buy;
        Price_t price = 0;
    };

    struct OpenOrderEntry {
        uint8_t       venue     = 0;
        uint32_t      agent     = 0;
        AccountId     account   = 0;
        ClientOrderId client_id = 0;
        Side_t        side      = Side_t::Buy;
        Price_t       price     = 0;
        Qty_t         remaining = 0;
    };

    void apply_fill(uint32_t agent, const Fill& f) {
        PortfolioState::apply_fill(portfolios_[agent].venues[f.venue], f.side, f.price, f.qty);
    }

    uint32_t agent_of(AccountId account) const {
        auto it = account_to_agent_.find(account);
        return it == account_to_agent_.end() ? 0u : it->second;
    }

    size_t n_agents_ = 0;
    std::map<AccountId, uint32_t>               account_to_agent_;
    std::vector<std::vector<Fill>>              pending_fills_;
    std::vector<std::vector<OrderUpdate>>       pending_updates_;
    std::map<OrderKey, PendingEntry>            pending_;
    std::map<OrderKey, OpenOrderEntry>          open_orders_;
    std::vector<PortfolioState>                 portfolios_;
};

inline void FillRouter::route_trades(uint8_t venue,
                                     const std::vector<TradeEvent>& trades) {
    for (const TradeEvent& t : trades) {
        const Side_t maker_side =
            (t.taker_side == Side_t::Buy) ? Side_t::Sell : Side_t::Buy;

        const uint32_t maker_agent = agent_of(t.maker_account_id);
        Fill mf{venue, t.maker_client_order_id, maker_side, t.price, t.qty, true};
        pending_fills_[maker_agent].push_back(mf);
        apply_fill(maker_agent, mf);

        auto it = open_orders_.find(
            OrderKey{venue, t.maker_account_id, t.maker_client_order_id});
        if (it != open_orders_.end()) {
            if (t.maker_cleared) {
                pending_updates_[maker_agent].push_back(
                    OrderUpdate{OrderUpdate::Kind::Filled, venue,
                                t.maker_client_order_id, RejectReason::None, 0});
                open_orders_.erase(it);
            } else {
                it->second.remaining =
                    it->second.remaining > t.qty
                        ? static_cast<Qty_t>(it->second.remaining - t.qty)
                        : 0;
                if (it->second.remaining == 0) {
                    pending_updates_[maker_agent].push_back(
                        OrderUpdate{OrderUpdate::Kind::Filled, venue,
                                    t.maker_client_order_id, RejectReason::None, 0});
                    open_orders_.erase(it);
                }
            }
        }

        const uint32_t taker_agent = agent_of(t.taker_account_id);
        Fill tf{venue, t.taker_client_order_id, t.taker_side, t.price, t.qty, false};
        pending_fills_[taker_agent].push_back(tf);
        apply_fill(taker_agent, tf);
    }
}

inline void FillRouter::route_replies(
    uint8_t venue, const std::vector<InputReplyEvent>& replies) {
    for (const InputReplyEvent& r : replies) {
        const uint32_t agent = agent_of(r.account_id);
        const OrderKey key{venue, r.account_id, r.client_order_id};
        const RejectReason reason = reject_reason_from_issue(r.reject_reason);

        // Checker / IDallocator / core rejection (kind is NOOP on the wire).
        if (r.reject_reason != OPIssue::Default) {
            pending_.erase(key);
            pending_updates_[agent].push_back(
                OrderUpdate{OrderUpdate::Kind::Rejected, venue,
                            r.client_order_id, reason, 0});
            continue;
        }
        if (r.kind == OPKind::NOOP) {
            pending_.erase(key);
            continue;
        }

        if (r.kind == OPKind::Limit) {
            if (r.rested_qty > 0) {
                auto pit = pending_.find(key);
                if (pit != pending_.end()) {
                    open_orders_[key] = OpenOrderEntry{
                        venue, agent, r.account_id, r.client_order_id,
                        pit->second.side, pit->second.price, r.rested_qty};
                }
                pending_updates_[agent].push_back(
                    OrderUpdate{OrderUpdate::Kind::Accepted, venue,
                                r.client_order_id, RejectReason::None, r.rested_qty});
            } else {
                pending_updates_[agent].push_back(
                    OrderUpdate{OrderUpdate::Kind::Filled, venue,
                                r.client_order_id, RejectReason::None, 0});
            }
            pending_.erase(key);
        } else if (r.kind == OPKind::Market) {
            pending_updates_[agent].push_back(
                OrderUpdate{r.filled_qty > 0 ? OrderUpdate::Kind::Filled
                                             : OrderUpdate::Kind::Cancelled,
                            venue, r.client_order_id, RejectReason::None, 0});
            pending_.erase(key);
        } else if (r.kind == OPKind::Cancel) {
            if (r.cancel_found) {
                open_orders_.erase(key);
                pending_updates_[agent].push_back(
                    OrderUpdate{OrderUpdate::Kind::Cancelled, venue,
                                r.client_order_id, RejectReason::None, 0});
            } else {
                pending_updates_[agent].push_back(
                    OrderUpdate{OrderUpdate::Kind::Rejected, venue,
                                r.client_order_id,
                                RejectReason::UnknownCancelTarget, 0});
            }
        }
    }
}

inline void FillRouter::route_ref(uint8_t venue, const std::vector<RefFill>& fills) {
    (void)venue;
    for (const RefFill& rf : fills) {
        pending_fills_[rf.agent].push_back(rf.fill);
        apply_fill(rf.agent, rf.fill);
    }
}

inline void FillRouter::finalize_tick(uint64_t tick,
                                      const std::vector<AgentBase*>& agents,
                                      const MarketView& agent_view) {
    for (size_t i = 0; i < agents.size(); ++i) {
        AgentBase* a = agents[i];

        a->priv_in.tick = tick;
        a->priv_in.fills = std::move(pending_fills_[i]);
        a->priv_in.updates = std::move(pending_updates_[i]);
        pending_fills_[i].clear();
        pending_updates_[i].clear();

        a->priv_in.open_orders.clear();
        for (const auto& [k, o] : open_orders_) {
            if (o.agent == i) {
                a->priv_in.open_orders.push_back(
                    OpenOrder{o.venue, o.client_id, o.side, o.price, o.remaining});
            }
        }

        a->portfolio_in.tick = tick;
        a->portfolio_in.venues = portfolios_[i].venues;

        for (size_t v = 0; v < MAX_VENUES; ++v) {
            const VenueView& vv = agent_view.venues[v];
            if (vv.bid_price != 0 && vv.ask_price != 0) {
                const double mark = (static_cast<double>(vv.bid_price) +
                                     static_cast<double>(vv.ask_price)) / 2.0;
                a->portfolio_in.venues[v].unrealized_pnl =
                    static_cast<double>(a->portfolio_in.venues[v].position) *
                    (mark - a->portfolio_in.venues[v].avg_cost);
            } else {
                a->portfolio_in.venues[v].unrealized_pnl = 0.0;
            }
        }
    }
}

} // namespace av
