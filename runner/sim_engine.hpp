#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
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
#include "agents/strategies/market_maker.hpp"
#include "agents/strategies/momentum_strategy.hpp"

namespace av {

enum class Scenario : uint8_t {
    Default,   // noise traders + the three sample strategies
    MmArb,     // N market makers + N reference arbitrageurs + M noise traders
};

// First mm-arb model:
//   N MMs (p1, s1) and N arbs (p2, s2), both with cash C and base inventory B;
//   M noise traders with unlimited cash/base; one uniform market-data delay t.
// The real venue is the LOB under study; the synthetic reference venue is the
// second, exogenous price (it has no MM / price impact yet).
struct MmArbConfig {
    size_t n_mm    = 10;
    size_t n_arb   = 10;
    size_t n_noise = 100;

    double p1 = 2.0;   // mean MM half-spread, ticks
    double s1 = 0.0;   // MM half-spread std dev
    double p2 = 1.0;   // mean arb edge threshold, ticks
    double s2 = 0.0;   // arb edge std dev

    uint64_t cash           = 20'000'000; // C (large enough for the B band by default)
    uint64_t base           = 1'000;      // B: initial base / inventory cap
    uint64_t position_limit = 0;          // 0 => 2*B (checker absolute cap)
    uint32_t latency        = 1;          // t: uniform market-data delay

    Qty_t mm_lot            = 10;
    Qty_t arb_lot           = 10;
    double mm_gamma         = 0.0;        // inventory skew, ticks per unit
    int mm_max_rest_ticks   = 5;

    double noise_lambda_new    = 0.10;
    double noise_lambda_cancel = 0.05;
    double noise_sigma         = 5.0;
    double noise_limit_ratio   = 0.9;
    size_t noise_max_open      = 8;
    Qty_t  noise_qty_min       = 1;
    Qty_t  noise_qty_max       = 100;
};

struct SimConfig {
    uint64_t    ticks         = 10000;
    uint32_t    market_delay  = 1;        // >= 1 (there must be a pending view)
    uint64_t    substep_max   = 1000000;  // per agent tick (drain-to-empty guard)
    size_t      threads       = 1;
    uint64_t    seed          = 42;
    std::string out_dir       = "build/sim";
    bool        record        = true;
    size_t      noise_agents  = 100;      // Default scenario, plus 3 strategies
    Scenario    scenario      = Scenario::Default;
    MmArbConfig mm_arb;
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

        if (cfg.scenario == Scenario::MmArb) setup_mm_arb();
        else                                  setup_default();

        for (auto& up : agent_storage) agents.push_back(up.get());

        auto sig = std::make_unique<StandardSignals>();
        runner.add_signal(sig.get());
        signal_storage.push_back(std::move(sig));

        for (AgentBase* a : agents) runner.add(a);
        runner.set_threads(cfg.threads);

        reset_all();
    }

    // Default population: noise traders (class 0) + MACD / momentum / arb
    // (classes 1 / 2 / 3).
    void setup_default() {
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

    // First MM-Arb model: noise (class 0), market makers (class 1) and
    // reference arbitrageurs (class 2). All agents see the same delay t.
    void setup_mm_arb() {
        const MmArbConfig& p = cfg.mm_arb;
        cfg.market_delay = std::max<uint32_t>(1, p.latency);
        const uint64_t pos_limit =
            p.position_limit ? p.position_limit : 2 * p.base;

        uint32_t id = 0;
        for (size_t i = 0; i < p.n_noise; ++i, ++id) {
            RandomTrader::Config rc;
            rc.lambda_new    = p.noise_lambda_new;
            rc.lambda_cancel = p.noise_lambda_cancel;
            rc.sigma         = p.noise_sigma;
            rc.limit_ratio   = p.noise_limit_ratio;
            rc.max_open      = p.noise_max_open;
            rc.qty_min       = p.noise_qty_min;
            rc.qty_max       = p.noise_qty_max;
            rc.venue         = real.index;
            rc.sym           = real.sym;
            agent_storage.push_back(std::make_unique<RandomTrader>(
                id, 3'000'000ull + i, 0u, rc, mix_seed(cfg.seed, i)));
        }

        for (size_t i = 0; i < p.n_mm; ++i, ++id) {
            MarketMakerStrategy::Config c;
            c.half_spread    = p.p1;
            c.spread_sigma   = p.s1;
            c.lot            = p.mm_lot;
            c.max_inventory  = static_cast<Qty_t>(p.base);
            c.gamma          = p.mm_gamma;
            c.max_rest_ticks = p.mm_max_rest_ticks;
            c.venue          = real.index;
            c.sym            = real.sym;
            c.seed           = mix_seed(cfg.seed, 1'000 + i);

            auto a = std::make_unique<MarketMakerStrategy>(
                id, 1'000'000ull + i, 1u, c);
            a->account_cfg.cash           = p.cash;
            a->account_cfg.base           = p.base;
            a->account_cfg.position_limit = pos_limit;
            agent_storage.push_back(std::move(a));
        }

        for (size_t i = 0; i < p.n_arb; ++i, ++id) {
            ArbitrageStrategy::Config c;
            c.venue_real = real.index;
            c.venue_ref  = ref.index;
            c.sym        = real.sym;
            c.lot        = p.arb_lot;
            c.max_pos    = static_cast<int64_t>(p.base);
            c.edge       = static_cast<int64_t>(std::llround(p.p2));
            c.edge_sigma = p.s2;
            c.seed       = mix_seed(cfg.seed, 2'000 + i);

            auto a = std::make_unique<ArbitrageStrategy>(
                id, 2'000'000ull + i, 2u, c);
            a->account_cfg.cash           = p.cash;
            a->account_cfg.base           = p.base;
            a->account_cfg.position_limit = pos_limit;
            agent_storage.push_back(std::move(a));
        }
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
        if (cfg.scenario == Scenario::MmArb) {
            const MmArbConfig& p = cfg.mm_arb;
            std::printf(
                "sim done: ticks=%llu agents=%zu (%zu noise + %zu mm + %zu arb) "
                "threads=%zu delay=%u p1=%.3f p2=%.3f\n",
                static_cast<unsigned long long>(cfg.ticks), agents.size(),
                p.n_noise, p.n_mm, p.n_arb, cfg.threads, cfg.market_delay,
                p.p1, p.p2);
        } else {
            std::printf(
                "sim done: ticks=%llu agents=%zu (%zu noise + %zu strategies) "
                "threads=%zu delay=%u\n",
                static_cast<unsigned long long>(cfg.ticks), agents.size(),
                cfg.noise_agents, agents.size() - cfg.noise_agents,
                cfg.threads, cfg.market_delay);
        }
        std::printf(
            "  real venue: trades=%llu volume=%llu open_orders=%zu\n"
            "  ref  venue: trades=%llu volume=%llu\n",
            static_cast<unsigned long long>(real.stats.trade_count),
            static_cast<unsigned long long>(real.stats.total_volume),
            router.open_order_count(),
            static_cast<unsigned long long>(ref.stats.trade_count),
            static_cast<unsigned long long>(ref.stats.total_volume));
    }

    // PnL of one agent class over all venues (or one venue). Realized PnL is
    // taken from the fill-based portfolios; the open inventory is marked at the
    // final mid. The initial cash/base is not part of the portfolio, so this is
    // the trading PnL relative to the common initial endowment.
    struct PnLSummary {
        size_t n          = 0;
        double realized   = 0.0;
        double unrealized = 0.0;
        double total      = 0.0;
    };

    // The reference venue is the exogenous fair value in the first model.
    double ref_mark() const {
        return 0.5 * (static_cast<double>(ref.ref_bid) +
                      static_cast<double>(ref.ref_ask));
    }

    // The real venue's current mark: mid, else last trade, else the anchor.
    double real_mark() const {
        const VenueView v = real.view();
        if (v.bid_price != 0 && v.ask_price != 0) {
            return 0.5 * (static_cast<double>(v.bid_price) +
                          static_cast<double>(v.ask_price));
        }
        if (v.last_price != 0) return static_cast<double>(v.last_price);
        return static_cast<double>(REF_ANCHOR_PRICE);
    }

    // If `common_mark` is set, every venue's open inventory is marked at that
    // price. Cross-market arbitrageurs should use the same asset mark for both
    // legs (the real mid here); otherwise the real/ref basis is mis-attributed.
    PnLSummary pnl_of_class(uint32_t class_id, int venue = -1,
                            std::optional<double> common_mark = std::nullopt) const {
        std::array<double, MAX_VENUES> marks{};
        std::array<bool, MAX_VENUES> valid{};

        marks[0] = real_mark();
        valid[0] = true;
        marks[1] = 0.5 * (static_cast<double>(ref.ref_bid) +
                          static_cast<double>(ref.ref_ask));
        valid[1] = true;

        PnLSummary s;
        const size_t begin = venue < 0 ? 0 : static_cast<size_t>(venue);
        const size_t end   = venue < 0 ? MAX_VENUES : begin + 1;
        if (begin >= MAX_VENUES) return s;

        for (size_t i = 0; i < agents.size(); ++i) {
            if (agents[i]->class_id != class_id) continue;
            ++s.n;
            const PortfolioState& p = router.portfolio(static_cast<uint32_t>(i));
            for (size_t v = begin; v < end; ++v) {
                s.realized += p.venues[v].realized_pnl;
                const double mark = common_mark.value_or(marks[v]);
                if (common_mark.has_value() || valid[v]) {
                    s.unrealized +=
                        static_cast<double>(p.venues[v].position) *
                        (mark - p.venues[v].avg_cost);
                }
            }
        }
        s.total = s.realized + s.unrealized;
        return s;
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
