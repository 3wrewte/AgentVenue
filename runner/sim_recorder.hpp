#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "agents/agent_base.hpp"
#include "agents/agent_types.hpp"
#include "io/csvrecorder.hpp"
#include "runner/fill_router.hpp"

namespace av {

// Reuses OoOobookCPP's buffered CSV sink (OutputStreamToCSV).
class SimRecorder {
public:
    bool enabled = false;

    void configure(const std::string& dir, bool enable) {
        enabled = enable;
        if (!enabled) return;
        market_ = std::make_unique<OutputStreamToCSV>(
            dir + "/market.csv",
            "tick,venue,sym,bid,ask,mid,spread,last,last_qty,trade_count,volume");
        agents_ = std::make_unique<OutputStreamToCSV>(
            dir + "/agents.csv",
            "tick,agent,class,venue,position,cash,avg_cost,realized,unrealized,open_orders");
        orders_ = std::make_unique<OutputStreamToCSV>(
            dir + "/agent_orders.csv",
            "tick,agent,venue,kind,side,price,qty,client_id,status,reason");
        ref_fills_ = std::make_unique<OutputStreamToCSV>(
            dir + "/ref_fills.csv",
            "tick,agent,client_id,side,price,qty");
        replies_ = std::make_unique<OutputStreamToCSV>(
            dir + "/replies.csv",
            "tick,venue,seq,account,client_id,kind,reject,detail,"
            "filled,rested,discarded,cancel_found,cancel_qty");
    }

    void reset() {
        if (!enabled) return;
        market_->reset();
        agents_->reset();
        orders_->reset();
        ref_fills_->reset();
        replies_->reset();
    }

    void prepare() {
        if (!enabled) return;
        market_->prepare();
        agents_->prepare();
        orders_->prepare();
        ref_fills_->prepare();
        replies_->prepare();
    }

    void commit() {
        if (!enabled) return;
        market_->commit();
        agents_->commit();
        orders_->commit();
        ref_fills_->commit();
        replies_->commit();
    }

    void finish() {
        if (!enabled) return;
        market_->finish();
        agents_->finish();
        orders_->finish();
        ref_fills_->finish();
        replies_->finish();
    }

    void record_market(uint64_t tick, const MarketView& view) {
        if (!enabled || !view.valid) return;
        for (size_t v = 0; v < MAX_VENUES; ++v) {
            const VenueView& vv = view.venues[v];
            const uint64_t mid = (vv.bid_price != 0 && vv.ask_price != 0)
                                     ? (static_cast<uint64_t>(vv.bid_price) +
                                        static_cast<uint64_t>(vv.ask_price)) / 2
                                     : 0;
            const uint64_t spread = (vv.bid_price != 0 && vv.ask_price != 0 &&
                                     vv.ask_price > vv.bid_price)
                                        ? vv.ask_price - vv.bid_price
                                        : 0;
            std::string line;
            line.reserve(96);
            line += std::to_string(tick);            line += ',';
            line += std::to_string(v);               line += ',';
            line += std::to_string(vv.sym);          line += ',';
            line += std::to_string(vv.bid_price);    line += ',';
            line += std::to_string(vv.ask_price);    line += ',';
            line += std::to_string(mid);             line += ',';
            line += std::to_string(spread);          line += ',';
            line += std::to_string(vv.last_price);   line += ',';
            line += std::to_string(vv.last_qty);     line += ',';
            line += std::to_string(vv.trade_count);  line += ',';
            line += std::to_string(vv.total_volume);
            market_->record(std::move(line));
        }
    }

    void record_agents(uint64_t tick, const std::vector<AgentBase*>& agents) {
        if (!enabled) return;
        for (const AgentBase* a : agents) {
            for (size_t v = 0; v < MAX_VENUES; ++v) {
                const VenuePortfolio& p = a->portfolio_in.venues[v];
                std::string line;
                line.reserve(128);
                line += std::to_string(tick);          line += ',';
                line += std::to_string(a->id);         line += ',';
                line += std::to_string(a->class_id);   line += ',';
                line += std::to_string(v);             line += ',';
                line += std::to_string(p.position);    line += ',';
                line += std::to_string(p.cash);        line += ',';
                line += std::to_string(p.avg_cost);    line += ',';
                line += std::to_string(p.realized_pnl);line += ',';
                line += std::to_string(p.unrealized_pnl); line += ',';
                line += std::to_string(a->priv_in.open_orders.size());
                agents_->record(std::move(line));
            }
        }
    }

    void record_order(uint64_t tick, uint32_t agent, const AgentOrder& o,
                      bool accepted, RejectReason reason) {
        if (!enabled) return;
        const char* kind = "limit";
        if (o.kind == OPKind::Market) kind = "market";
        else if (o.kind == OPKind::Cancel) kind = "cancel";
        std::string line;
        line.reserve(96);
        line += std::to_string(tick);             line += ',';
        line += std::to_string(agent);            line += ',';
        line += std::to_string(o.venue);          line += ',';
        line += kind;                             line += ',';
        line += (o.side == Side_t::Buy ? "B" : "S"); line += ',';
        line += std::to_string(o.price);          line += ',';
        line += std::to_string(o.qty);            line += ',';
        line += std::to_string(o.client_id);      line += ',';
        line += (accepted ? "accepted" : "rejected"); line += ',';
        line += reject_reason_name(reason);
        orders_->record(std::move(line));
    }

    void record_replies(uint64_t tick, uint8_t venue,
                        const std::vector<InputReplyEvent>& replies) {
        if (!enabled) return;
        for (const InputReplyEvent& r : replies) {
            const char* kind = "noop";
            if (r.kind == OPKind::Limit)  kind = "limit";
            else if (r.kind == OPKind::Market) kind = "market";
            else if (r.kind == OPKind::Cancel) kind = "cancel";

            std::string line;
            line.reserve(128);
            line += std::to_string(tick);              line += ',';
            line += std::to_string(venue);             line += ',';
            line += std::to_string(r.seq);             line += ',';
            line += std::to_string(r.account_id);      line += ',';
            line += std::to_string(r.client_order_id); line += ',';
            line += kind;                              line += ',';
            line += reject_reason_name(reject_reason_from_issue(r.reject_reason));
            line += ',';
            line += std::to_string(r.reject_detail);   line += ',';
            line += std::to_string(r.filled_qty);      line += ',';
            line += std::to_string(r.rested_qty);      line += ',';
            line += std::to_string(r.discarded_qty);   line += ',';
            line += (r.cancel_found ? '1' : '0');      line += ',';
            line += std::to_string(r.cancel_qty);
            replies_->record(std::move(line));
        }
    }

    void record_ref_fill(uint64_t tick, const RefFill& rf) {
        if (!enabled) return;
        std::string line;
        line.reserve(64);
        line += std::to_string(tick);                    line += ',';
        line += std::to_string(rf.agent);                line += ',';
        line += std::to_string(rf.fill.client_id);       line += ',';
        line += (rf.fill.side == Side_t::Buy ? "B" : "S"); line += ',';
        line += std::to_string(rf.fill.price);           line += ',';
        line += std::to_string(rf.fill.qty);
        ref_fills_->record(std::move(line));
    }

private:
    std::unique_ptr<OutputStreamToCSV> market_;
    std::unique_ptr<OutputStreamToCSV> agents_;
    std::unique_ptr<OutputStreamToCSV> orders_;
    std::unique_ptr<OutputStreamToCSV> ref_fills_;
    std::unique_ptr<OutputStreamToCSV> replies_;
};

} // namespace av
