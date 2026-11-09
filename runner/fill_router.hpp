#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

#include "agents/agent_base.hpp"
#include "agents/agent_types.hpp"
#include "bookkeeping/IDallocator.hpp"
#include "core/instructions.hpp"
#include "runner/portfolio.hpp"

namespace av {

struct RefFill {
    uint32_t agent = 0;
    Fill     fill;
};

// Turns canonical events into per-agent fills / order updates / open-order
// state, and applies fills to the spot portfolios.
//
// The core's Insert event only carries its address; the resting volume is
// recovered as `ctx.qty - (taker volume of this OP)`. The taker volume is
// accumulated from the TakeLine events of the same OP, which commit before
// the Insert event (a single OP is processed per core tick).
class FillRouter {
public:
    void reset(size_t n_agents) {
        n_agents_ = n_agents;
        pending_fills_.assign(n_agents, {});
        pending_updates_.assign(n_agents, {});
        portfolios_.assign(n_agents, PortfolioState{});
        open_orders_.clear();
        account_to_agent_.clear();
    }

    void set_accounts(const std::vector<AgentBase*>& agents) {
        account_to_agent_.clear();
        for (size_t i = 0; i < agents.size(); ++i) {
            account_to_agent_[agents[i]->account] = static_cast<uint32_t>(i);
        }
    }

    void route_real(uint8_t venue, IDallocator& id_alloc,
                    const std::vector<CanonicalEvent>& events);

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
    static uint64_t key(uint8_t venue, OrderID_t id) {
        return (static_cast<uint64_t>(venue) << 32) | static_cast<uint64_t>(id);
    }

    struct OpenOrderEntry {
        uint8_t       venue     = 0;
        uint32_t      agent     = 0;
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
    std::unordered_map<AccountId, uint32_t> account_to_agent_;
    std::vector<std::vector<Fill>>          pending_fills_;
    std::vector<std::vector<OrderUpdate>>   pending_updates_;
    std::unordered_map<uint64_t, OpenOrderEntry> open_orders_;  // (venue, internal id)
    std::vector<PortfolioState>             portfolios_;
};

inline void FillRouter::route_real(uint8_t venue, IDallocator& id_alloc,
                                   const std::vector<CanonicalEvent>& events) {
    std::unordered_map<OrderID_t, Qty_t> taker_filled;

    for (const CanonicalEvent& ev : events) {
        switch (ev.kind) {
        case UopKind::TakeLine: {
            const auto& take = std::get<TakeLineResult>(ev.payload);
            const auto  taker_ctx = id_alloc.get(ev.id);

            for (size_t slot = 0; slot < take.trade_vol_vec.size(); ++slot) {
                const Qty_t vol = take.trade_vol_vec[slot];
                if (vol == 0) continue;
                const OrderID_t maker_id = take.trade_id_vec[slot];
                const auto maker_ctx = id_alloc.get(maker_id);

                const Price_t trade_price = maker_ctx ? maker_ctx->Price
                                                      : (taker_ctx ? taker_ctx->Price : 0);

                if (maker_ctx) {
                    const uint32_t maker_agent = agent_of(maker_ctx->AccountId);
                    Fill mf{venue, maker_ctx->ClientOrderId, maker_ctx->Side,
                            trade_price, vol, true};
                    pending_fills_[maker_agent].push_back(mf);
                    apply_fill(maker_agent, mf);

                    auto it = open_orders_.find(key(venue, maker_id));
                    if (it != open_orders_.end()) {
                        it->second.remaining = it->second.remaining > vol
                                                   ? static_cast<Qty_t>(it->second.remaining - vol)
                                                   : 0;
                        if (it->second.remaining == 0) {
                            pending_updates_[maker_agent].push_back(
                                OrderUpdate{OrderUpdate::Kind::Filled, venue,
                                            it->second.client_id, RejectReason::None, 0});
                            open_orders_.erase(it);
                        }
                    }
                }

                if (taker_ctx) {
                    const uint32_t taker_agent = agent_of(taker_ctx->AccountId);
                    Fill tf{venue, taker_ctx->ClientOrderId, taker_ctx->Side,
                            trade_price, vol, false};
                    pending_fills_[taker_agent].push_back(tf);
                    apply_fill(taker_agent, tf);
                }

                taker_filled[ev.id] += vol;
            }
            break;
        }
        case UopKind::InsertSlot: {
            const auto ctx = id_alloc.get(ev.id);
            if (!ctx) break;
            const uint32_t agent = agent_of(ctx->AccountId);
            const Qty_t filled = taker_filled.count(ev.id) ? taker_filled[ev.id] : 0;
            const Qty_t rest = ctx->qty > filled ? static_cast<Qty_t>(ctx->qty - filled) : 0;
            const auto addr = std::get<CanonicalAddr_t>(ev.payload);
            const Price_t price = CanonicalAddrUnpack(addr).price;

            open_orders_[key(venue, ev.id)] = OpenOrderEntry{
                venue, agent, ctx->ClientOrderId, ctx->Side, price, rest};
            pending_updates_[agent].push_back(
                OrderUpdate{OrderUpdate::Kind::Accepted, venue,
                            ctx->ClientOrderId, RejectReason::None, rest});
            break;
        }
        case UopKind::CancelSlot: {
            const auto& cr = std::get<CancelResult>(ev.payload);
            const auto target = id_alloc.get(cr.order_id);
            const auto cancel_ctx = id_alloc.get(ev.id);

            const uint32_t agent = target ? agent_of(target->AccountId)
                                          : (cancel_ctx ? agent_of(cancel_ctx->AccountId) : 0u);
            const ClientOrderId cid = target ? target->ClientOrderId
                                             : (cancel_ctx ? cancel_ctx->ClientOrderId : 0);

            open_orders_.erase(key(venue, cr.order_id));
            pending_updates_[agent].push_back(
                OrderUpdate{OrderUpdate::Kind::Cancelled, venue, cid, RejectReason::None, 0});
            break;
        }
        case UopKind::End: {
            // Reached for a cancel whose target is unknown (NOOP) or for an
            // order that was downgraded to NOOP by the bookkeeping layer.
            const auto ctx = id_alloc.get(ev.id);
            if (!ctx) break;
            const uint32_t agent = agent_of(ctx->AccountId);
            if (ctx->kind == OPKind::Cancel) {
                pending_updates_[agent].push_back(
                    OrderUpdate{OrderUpdate::Kind::Cancelled, venue,
                                ctx->ClientOrderId, RejectReason::UnknownCancelTarget, 0});
            } else {
                pending_updates_[agent].push_back(
                    OrderUpdate{OrderUpdate::Kind::Rejected, venue,
                                ctx->ClientOrderId, RejectReason::Other, 0});
            }
            break;
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
