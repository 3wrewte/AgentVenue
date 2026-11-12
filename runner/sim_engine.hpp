#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "agents/agent_base.hpp"
#include "agents/agent_types.hpp"
#include "agents/signal_base.hpp"
#include "agents/signals/standard.hpp"
#include "agents/strategies/random_trader.hpp"
#include "core/phase.hpp"
#include "runner/agent_runner.hpp"
#include "runner/delay_line.hpp"
#include "runner/fill_router.hpp"
#include "runner/order_source.hpp"
#include "runner/sim_recorder.hpp"
#include "runner/venue.hpp"

//The following packages are the added strategy see in agents/strategy
#include "agents/strategies/arbitrage_strategy.hpp"
#include "agents/strategies/macd_strategy.hpp"
#include "agents/strategies/momentum_strategy.hpp"

namespace av {

struct SimConfig {
    uint64_t    ticks         = 10000;
    uint32_t    market_delay  = 1;        // >= 1 (there must be a pending view)
    uint64_t    substep_max   = 1000000;  // per agent tick (drain-to-empty guard)
    size_t      threads       = 1;
    uint64_t    seed          = 42;
    std::string out_dir       = "build/sim";
    bool        record        = true;
    size_t      noise_agents  = 100;      // plus 3 fixed sample strategies
};

class SimEngine {
public:
    SimConfig cfg;

    RealVenue      real;
    SyntheticVenue ref;

    AgentRunner  runner;
    FillRouter   router;
    DelayLine<MarketView, 16> delay;
    SignalSet    signals_cur{};
    SignalSet    signals_next{};

    std::vector<AgentBase*>                    agents;
    std::vector<std::unique_ptr<AgentBase>>    agent_storage;
    std::vector<std::unique_ptr<SignalModule>> signal_storage;
    SimRecorder recorder;

    void setup() {
        if (cfg.record) std::filesystem::create_directories(cfg.out_dir);
        real.configure(0, 1, cfg.out_dir, cfg.record);
        ref.configure(1, 1, REF_ANCHOR_PRICE - REF_HALF_SPREAD,
                      REF_ANCHOR_PRICE + REF_HALF_SPREAD);
        recorder.configure(cfg.out_dir, cfg.record);

        const size_t half = cfg.noise_agents / 2;
        for (size_t i = 0; i < cfg.noise_agents; ++i) {
            const bool second = i >= half;
            RandomTrader::Config rc;
            rc.lambda_new    = (second ? 0.20 : 0.10) * (1.0 + 0.1 * static_cast<double>(i % 3));
            rc.lambda_cancel = (second ? 0.10 : 0.05);
            rc.sigma         = (second ? 10.0 : 5.0) * (1.0 + 0.1 * static_cast<double>(i % 5));
            rc.limit_ratio   = second ? 0.8 : 0.9;
            rc.max_open      = second ? 10 : 8;
            rc.venue         = 0;
            rc.sym           = real.sym;

            const uint64_t seed = mix_seed(cfg.seed, i);
            auto agent = std::make_unique<RandomTrader>(
                static_cast<uint32_t>(i), 1'000'000ull + i, 0u, rc, seed);
            agent_storage.push_back(std::move(agent));
        }
                // --- three sample strategies on top of the noise traders ---
        {
            const uint32_t base = static_cast<uint32_t>(cfg.noise_agents);

            MacdStrategy::Config mcfg;
            mcfg.venue = real.index;
            mcfg.sym   = real.sym;
            agent_storage.push_back(std::make_unique<MacdStrategy>(
                base + 0u, 2'000'000ull, 1u, mcfg));

            MomentumStrategy::Config ncfg;
            ncfg.venue = real.index;
            ncfg.sym   = real.sym;
            agent_storage.push_back(std::make_unique<MomentumStrategy>(
                base + 1u, 2'000'001ull, 2u, ncfg));

            ArbitrageStrategy::Config acfg;
            acfg.venue_real = real.index;
            acfg.venue_ref  = ref.index;
            acfg.sym        = real.sym;
            agent_storage.push_back(std::make_unique<ArbitrageStrategy>(
                base + 2u, 2'000'002ull, 3u, acfg));
        }
        for (auto& up : agent_storage) agents.push_back(up.get());

        auto sig = std::make_unique<StandardSignals>();
        runner.add_signal(sig.get());
        signal_storage.push_back(std::move(sig));

        for (AgentBase* a : agents) runner.add(a);
        runner.set_threads(cfg.threads);

        reset_all();
    }

    void reset_all() {
        std::unordered_map<AccountId_t, AccountState> init_states;
        for (AgentBase* a : agents) {
            AccountState st;
            st.cash = a->account_cfg.cash;
            st.base = a->account_cfg.base;
            st.position_limit = a->account_cfg.position_limit;
            init_states[a->account] = st;
        }
        real.reset(std::move(init_states));
        ref.reset();
        delay.reset();
        signals_cur = SignalSet{};
        signals_next = SignalSet{};
        for (AgentBase* a : agents) a->reset();
        router.reset(agents.size());
        router.set_accounts(agents);
        recorder.reset();
    }

    void run() {
        for (uint64_t tick = 0; tick < cfg.ticks; ++tick) {
            recorder.prepare();

            high_freq_phase(tick);

            // Reference venue fills the orders queued on the previous gearshift.
            auto ref_fills = ref.step();
            for (const RefFill& rf : ref_fills) recorder.record_ref_fill(tick, rf);
            router.route_ref(ref.index, ref_fills);

            // Low-frequency switch: publish the book, compute next-cycle
            // signals, and deliver the delayed view + private state.
            MarketView view;
            view.tick = tick;
            view.valid = true;
            view.venues[0] = real.view();
            view.venues[1] = ref.view(tick);
            delay.publish(tick, view);
            recorder.record_market(tick, view);

            signals_next = signals_cur;

            const MarketView& agent_view =
                delay.at(static_cast<int64_t>(tick) - static_cast<int64_t>(cfg.market_delay));
            for (AgentBase* a : agents) {
                a->mkt_in = agent_view;
                a->signals_in = signals_cur;
            }
            router.finalize_tick(tick, agents, agent_view);
            for (AgentBase* a : agents) {
                a->priv_in.account = real.account_view(a->account);
            }
            recorder.record_agents(tick, agents);

            // RTL phases. Signals and agents are independent; the signal
            // modules read the view the agents will see next tick.
            const MarketView& future =
                delay.at(static_cast<int64_t>(tick) + 1 - static_cast<int64_t>(cfg.market_delay));

            global_phase = Phase::Prepare;
            runner.tick_prepare();
            global_phase = Phase::Comb;
            runner.tick_comb(future, signals_next);
            global_phase = Phase::Commit;
            runner.tick_commit();
            signals_cur = signals_next;

            // Gearshift: collect committed orders and enqueue them for the
            // next tick's high-frequency core sub-ticks.
            gearshift(tick);

            recorder.commit();
        }

        real.finish();
        recorder.finish();
    }

    void print_summary() const {
        std::printf(
            "sim done: ticks=%llu agents=%zu (%zu noise + %zu strategies) "
            "threads=%zu delay=%u\n"
            "  real venue: trades=%llu volume=%llu open_orders=%zu\n"
            "  ref  venue: trades=%llu volume=%llu\n",
            static_cast<unsigned long long>(cfg.ticks), agents.size(),
            cfg.noise_agents, agents.size() - cfg.noise_agents,
            cfg.threads, cfg.market_delay,
            static_cast<unsigned long long>(real.stats.trade_count),
            static_cast<unsigned long long>(real.stats.total_volume),
            router.open_order_count(),
            static_cast<unsigned long long>(ref.stats.trade_count),
            static_cast<unsigned long long>(ref.stats.total_volume));
    }

private:
    static uint64_t mix_seed(uint64_t seed, uint64_t i) {
        uint64_t x = seed + 0x9e3779b97f4a7c15ull * (i + 1);
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ull;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebull;
        x ^= x >> 31;
        return x;
    }

    void high_freq_phase(uint64_t tick) {
        auto not_idle = [&] {
            return !real.source.empty() || !real.op_pipeline_idle();
        };
        for (uint64_t s = 0; s < cfg.substep_max && not_idle(); ++s) {
            global_phase = Phase::Prepare;
            real.prepare();
            global_phase = Phase::Comb;
            VenueEvents events = real.comb();
            global_phase = Phase::Commit;
            real.commit();
            router.route_trades(real.index, events.trades);
            router.route_replies(real.index, events.replies);
            recorder.record_replies(tick, real.index, events.replies);
        }
    }

    void gearshift(uint64_t tick) {
        for (size_t ai = 0; ai < agents.size(); ++ai) {
            AgentBase* a = agents[ai];
            AgentOutput& out = a->out_committed;
            for (size_t k = 0; k < out.n; ++k) {
                handle_order(tick, static_cast<uint32_t>(ai), out.orders[k]);
            }
            out.clear();
        }
    }

    void handle_order(uint64_t tick, uint32_t ai, const AgentOrder& o) {
        const RejectReason reason = validate(o);
        if (reason != RejectReason::None) {
            router.push_update(ai, OrderUpdate{OrderUpdate::Kind::Rejected, o.venue,
                                               o.client_id, reason, 0});
            recorder.record_order(tick, ai, o, false, reason);
            return;
        }

        if (o.venue == real.index) {
            AgentBase* a = agents[ai];
            OPcontext ctx{};
            ctx.kind          = o.kind;
            ctx.Seq           = real.next_seq++;
            ctx.ContractId    = real.sym;
            ctx.AccountId     = a->account;
            ctx.ClientOrderId = o.client_id;
            ctx.Side          = o.side;
            ctx.Price         = (o.kind == OPKind::Market)
                                    ? (o.side == Side_t::Buy ? Price_max : Price_min)
                                    : o.price;
            ctx.qty           = o.qty;
            ctx.reject_reason = OPIssue::Default;
            ctx.reject_detail = 0;
            if (o.kind == OPKind::Limit) {
                router.track_submission(o.venue, a->account, o.client_id,
                                        o.kind, o.side, o.price);
            }
            real.submit(ctx);
        } else {
            // Synthetic reference venue: filled on the next tick's high-freq
            // phase (one-cycle reg-out delay).
            ref.pending.push_back(SyntheticVenue::Pending{ai, o});
        }
        recorder.record_order(tick, ai, o, true, RejectReason::None);
    }

    RejectReason validate(const AgentOrder& o) const {
        if (o.venue >= MAX_VENUES) return RejectReason::InvalidVenue;
        if (o.kind != OPKind::Limit && o.kind != OPKind::Market && o.kind != OPKind::Cancel) {
            return RejectReason::Other;
        }

        if (o.kind == OPKind::Cancel) {
            // The core resolves the target; an unknown target comes back as a
            // regular InputReplyEvent (OPIssue::UnknownCancelTarget).
            if (o.venue != real.index) return RejectReason::UnknownCancelTarget;
            return RejectReason::None;
        }

        if (o.qty == 0) return RejectReason::InvalidQty;
        if (o.qty > MAX_AGENT_QTY) return RejectReason::QtyTooLarge;

        if (o.kind == OPKind::Limit) {
            if (o.price == 0 || o.price > Price_max) return RejectReason::InvalidPrice;
            if (o.venue == ref.index) {
                const bool crosses = (o.side == Side_t::Buy)
                                         ? (o.price >= ref.ref_ask)
                                         : (o.price <= ref.ref_bid);
                if (!crosses) return RejectReason::LimitNotCrossingRef;
            }
        }
        return RejectReason::None;
    }
};

} // namespace av
