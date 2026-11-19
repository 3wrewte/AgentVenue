// Grid runner for MM + arb + CTA:
//   f(p1, p2, p3) = (MM PnL, arb PnL, CTA PnL)
// with N MMs, N arbs, N CTAs and M noise traders; C/B and delay t are common.
// p3 is the CTA's momentum entry threshold in bps.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "runner/sim_engine.hpp"

namespace {

struct Args {
    std::vector<double> p1s{1, 2, 3, 4};
    std::vector<double> p2s{0, 1, 2, 3};
    std::vector<double> p3s{2.0};

    size_t   n_mm      = 10;
    size_t   n_arb     = 10;
    size_t   n_cta     = 10;
    size_t   n_noise   = 100;
    uint64_t cash      = 20'000'000;
    uint64_t base      = 1'000;
    uint64_t pos_limit = 0;
    uint32_t latency   = 1;
    uint64_t ticks     = 2'000;
    uint64_t seed      = 42;
    size_t   threads   = 1;
    size_t   repeats   = 1;
    bool     record    = false;

    double s1       = 0.0;
    double s2       = 0.0;
    double gamma    = 0.0;
    int    mm_rest  = 5;
    Qty_t mm_lot    = 10;
    Qty_t arb_lot   = 10;
    Qty_t cta_lot   = 50;

    double arb_exit = 0.0;
    int    arb_hold = 100;
    Qty_t  arb_gap  = 1;
    bool   arb_net_edge = false;
    bool   arb_settle = true;
    double arb_fee = 0.0;
    uint32_t arb_latency = 0;

    size_t cta_lookback = 20;
    double cta_exit_bps = 0.0;
    double cta_entry_sigma = 0.0;
    size_t cta_lookback_sigma = 0;
    double cta_contrarian_frac = 0.0;

    std::string out_dir = "build/mm_arb_cta_grid";
};

void usage(const char* prog) {
    std::printf(
        "usage: %s [--p1 1,2,3] [--p2 0,1,2] [--p3 2] [--n-mm N] [--n-arb N]\n"
        "          [--n-cta N] [--noise N] [--cash C] [--base B] [--latency T]\n"
        "          [--ticks T] [--seed S] [--s1 X] [--s2 X] [--gamma X]\n"
        "          [--mm-lot Q] [--arb-lot Q] [--cta-lot Q] [--mm-rest K]\n"
        "          [--cta-lookback K] [--cta-exit X]\n"
        "          [--cta-entry-sigma X] [--cta-lookback-sigma K]\n"
        "          [--cta-contrarian-frac X]\n"
        "          [--arb-exit X] [--arb-hold K] [--arb-gap Q] [--arb-net-edge]\n"
        "          [--arb-no-settle] [--arb-fee-bps X] [--arb-latency T]\n"
        "          [--threads N] [--repeats R] [--out DIR] [--record]\n",
        prog);
    std::printf(
        "  p1 = mean MM half-spread (ticks), p2 = mean arb edge (ticks),\n"
        "  p3 = CTA momentum entry threshold (bps; smaller = more entries).\n"
        "  cta exit: close when the return crosses --cta-exit bps (default 0).\n"
        "  MM/arb/CTA accounts: cash C, base inventory B, position limit 2B.\n"
        "  Noise traders: unlimited cash/base.\n");
}

std::vector<double> parse_list(const std::string& s) {
    std::vector<double> out;
    size_t begin = 0;
    while (begin <= s.size()) {
        const size_t comma = s.find(',', begin);
        const std::string part = s.substr(
            begin, comma == std::string::npos ? std::string::npos : comma - begin);
        if (!part.empty()) out.push_back(std::stod(part));
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }
    return out;
}

std::string num(double v) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(6) << v;
    return os.str();
}

std::string tag(double v) {
    std::string s = num(v);
    for (char& c : s) {
        if (c == '.') c = 'p';
        else if (c == '-') c = 'm';
    }
    return s;
}

} // namespace

int main(int argc, char** argv) {
    Args args;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(2);
            }
            return argv[++i];
        };

        if (a == "--p1")             args.p1s = parse_list(next("--p1"));
        else if (a == "--p2")        args.p2s = parse_list(next("--p2"));
        else if (a == "--p3")        args.p3s = parse_list(next("--p3"));
        else if (a == "--n-mm")      args.n_mm = std::stoull(next("--n-mm"));
        else if (a == "--n-arb")     args.n_arb = std::stoull(next("--n-arb"));
        else if (a == "--n-cta")     args.n_cta = std::stoull(next("--n-cta"));
        else if (a == "--noise")     args.n_noise = std::stoull(next("--noise"));
        else if (a == "--cash")      args.cash = std::stoull(next("--cash"));
        else if (a == "--base")      args.base = std::stoull(next("--base"));
        else if (a == "--pos-limit") args.pos_limit = std::stoull(next("--pos-limit"));
        else if (a == "--latency")   args.latency = static_cast<uint32_t>(std::stoul(next("--latency")));
        else if (a == "--ticks")     args.ticks = std::stoull(next("--ticks"));
        else if (a == "--seed")      args.seed = std::stoull(next("--seed"));
        else if (a == "--threads")   args.threads = std::stoul(next("--threads"));
        else if (a == "--repeats")   args.repeats = std::stoul(next("--repeats"));
        else if (a == "--s1")        args.s1 = std::stod(next("--s1"));
        else if (a == "--s2")        args.s2 = std::stod(next("--s2"));
        else if (a == "--gamma")     args.gamma = std::stod(next("--gamma"));
        else if (a == "--mm-rest")   args.mm_rest = std::stoi(next("--mm-rest"));
        else if (a == "--mm-lot")    args.mm_lot = static_cast<Qty_t>(std::stoul(next("--mm-lot")));
        else if (a == "--arb-lot")   args.arb_lot = static_cast<Qty_t>(std::stoul(next("--arb-lot")));
        else if (a == "--cta-lot")   args.cta_lot = static_cast<Qty_t>(std::stoul(next("--cta-lot")));
        else if (a == "--cta-lookback") args.cta_lookback = std::stoul(next("--cta-lookback"));
        else if (a == "--cta-exit")  args.cta_exit_bps = std::stod(next("--cta-exit"));
        else if (a == "--cta-entry-sigma") args.cta_entry_sigma = std::stod(next("--cta-entry-sigma"));
        else if (a == "--cta-lookback-sigma") args.cta_lookback_sigma = std::stoul(next("--cta-lookback-sigma"));
        else if (a == "--cta-contrarian-frac") args.cta_contrarian_frac = std::stod(next("--cta-contrarian-frac"));
        else if (a == "--arb-exit")  args.arb_exit = std::stod(next("--arb-exit"));
        else if (a == "--arb-hold")  args.arb_hold = std::stoi(next("--arb-hold"));
        else if (a == "--arb-gap")   args.arb_gap = static_cast<Qty_t>(std::stoul(next("--arb-gap")));
        else if (a == "--arb-net-edge") args.arb_net_edge = true;
        else if (a == "--arb-no-settle") args.arb_settle = false;
        else if (a == "--arb-fee-bps") args.arb_fee = std::stod(next("--arb-fee-bps"));
        else if (a == "--arb-latency") args.arb_latency = static_cast<uint32_t>(std::stoul(next("--arb-latency")));
        else if (a == "--out")       args.out_dir = next("--out");
        else if (a == "--record")    args.record = true;
        else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    if (args.p1s.empty() || args.p2s.empty() || args.p3s.empty()) {
        std::fprintf(stderr, "p1/p2/p3 grids must be non-empty\n");
        return 2;
    }
    if (args.repeats == 0) args.repeats = 1;

    std::filesystem::create_directories(args.out_dir);
    const std::string path = args.out_dir + "/f_p1_p2_p3.csv";
    std::ofstream csv(path);
    if (!csv.is_open()) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return 2;
    }

    csv << "p1,p2,p3,n_mm,n_arb,n_cta,n_noise,"
           "mm_pnl,mm_realized,mm_unrealized,"
           "arb_pnl,arb_realized,arb_unrealized,arb_pnl_venue,"
           "cta_pnl,cta_realized,cta_unrealized,"
           "noise_pnl,mm_pnl_med,arb_pnl_med,cta_pnl_med,noise_pnl_med,"
           "real_check,"
           "trades,volume,latency,seed,repeats,"
           "arb_exit,arb_hold,arb_gap,arb_net_edge,cta_lot,cta_lookback,cta_exit_bps,"
           "cta_entry_sigma,cta_lookback_sigma,cta_contrarian_frac,"
           "cta_cs_std,cta_pos_frac,cta_best,cta_worst\n";

    const size_t total = args.p1s.size() * args.p2s.size() * args.p3s.size();
    size_t done = 0;

    for (double p1 : args.p1s) {
        for (double p2 : args.p2s) {
            for (double p3 : args.p3s) {
                double mm_total = 0, mm_real = 0, mm_unreal = 0;
                double arb_total = 0, arb_real = 0, arb_unreal = 0, arb_venue = 0;
                double cta_total = 0, cta_real = 0, cta_unreal = 0;
                double noise_total = 0, real_check = 0;
                uint64_t trades = 0, volume = 0;
                std::vector<double> mm_vals, arb_vals, cta_vals, noise_vals;
                std::vector<double> cta_cs_std_vals, cta_pos_frac_vals;
                std::vector<double> cta_best_vals, cta_worst_vals;

                for (size_t r = 0; r < args.repeats; ++r) {
                    av::SimEngine e;
                    e.cfg.record   = args.record;
                    e.cfg.out_dir  = args.out_dir + "/run_p1_" + tag(p1) +
                                     "_p2_" + tag(p2) + "_p3_" + tag(p3) +
                                     "_s" + std::to_string(args.seed + r);
                    e.cfg.ticks    = args.ticks;
                    e.cfg.seed     = args.seed + r;
                    e.cfg.threads  = args.threads;
                    e.cfg.scenario = av::Scenario::MmArb;

                    av::MmArbConfig& m = e.cfg.mm_arb;
                    m.n_mm           = args.n_mm;
                    m.n_arb          = args.n_arb;
                    m.n_cta          = args.n_cta;
                    m.n_noise        = args.n_noise;
                    m.p1             = p1;
                    m.s1             = args.s1;
                    m.p2             = p2;
                    m.s2             = args.s2;
                    m.cash           = args.cash;
                    m.base           = args.base;
                    m.position_limit = args.pos_limit;
                    m.latency        = args.latency;
                    m.mm_lot         = args.mm_lot;
                    m.arb_lot        = args.arb_lot;
                    m.cta_lot        = args.cta_lot;
                    m.mm_gamma       = args.gamma;
                    m.mm_max_rest_ticks = args.mm_rest;
                    m.arb_exit_profit    = args.arb_exit;
                    m.arb_max_hold_ticks = args.arb_hold;
                    m.arb_hedge_gap      = args.arb_gap;
                    m.arb_net_edge       = args.arb_net_edge;
                    m.arb_settle         = args.arb_settle;
                    m.arb_transfer_fee_bps = args.arb_fee;
                    m.arb_transfer_latency = args.arb_latency;
                    m.cta_entry_bps      = p3;
                    m.cta_exit_bps       = args.cta_exit_bps;
                    m.cta_lookback       = args.cta_lookback;
                    m.cta_entry_sigma_bps = args.cta_entry_sigma;
                    m.cta_lookback_sigma  = args.cta_lookback_sigma;
                    m.cta_contrarian_frac = args.cta_contrarian_frac;

                    e.setup();
                    e.run();

                    const av::SimEngine::PnLSummary mm    = e.pnl_of_class(1);
                    const av::SimEngine::PnLSummary arb   = e.pnl_of_class(2);
                    const av::SimEngine::PnLSummary arb_econ =
                        e.pnl_of_class(2, -1, e.ref_mark());
                    const av::SimEngine::PnLSummary cta   = e.pnl_of_class(3);
                    const av::SimEngine::PnLSummary noise = e.pnl_of_class(0);

                    mm_total  += mm.total;    mm_real  += mm.realized;    mm_unreal  += mm.unrealized;
                    arb_total += arb_econ.total;
                    arb_real  += arb_econ.realized;
                    arb_unreal += arb_econ.total - arb_econ.realized;
                    arb_venue += arb.total;
                    cta_total += cta.total;
                    cta_real  += cta.realized;
                    cta_unreal += cta.unrealized;
                    noise_total += noise.total;
                    mm_vals.push_back(mm.total);
                    arb_vals.push_back(arb_econ.total);
                    cta_vals.push_back(cta.total);
                    noise_vals.push_back(noise.total);

                    const std::vector<double> cta_agents =
                        e.agent_pnl_of_class(3);
                    if (!cta_agents.empty()) {
                        double mean = 0.0;
                        for (double x : cta_agents) mean += x;
                        mean /= static_cast<double>(cta_agents.size());
                        double var = 0.0;
                        size_t pos = 0;
                        double best = cta_agents.front();
                        double worst = cta_agents.front();
                        for (double x : cta_agents) {
                            const double d = x - mean;
                            var += d * d;
                            if (x > 0.0) ++pos;
                            best  = std::max(best, x);
                            worst = std::min(worst, x);
                        }
                        var /= static_cast<double>(cta_agents.size());
                        cta_cs_std_vals.push_back(std::sqrt(var));
                        cta_pos_frac_vals.push_back(
                            static_cast<double>(pos) /
                            static_cast<double>(cta_agents.size()));
                        cta_best_vals.push_back(best);
                        cta_worst_vals.push_back(worst);
                    }

                    const double common = 0.5 * (e.real_mark() + e.ref_mark());
                    real_check += e.pnl_of_class(1, -1, common).total +
                                  e.pnl_of_class(2, -1, common).total +
                                  e.pnl_of_class(3, -1, common).total +
                                  e.pnl_of_class(0, -1, common).total;
                    trades += e.real.stats.trade_count;
                    volume += e.real.stats.total_volume;
                }

                const double inv = 1.0 / static_cast<double>(args.repeats);
                mm_total *= inv; mm_real *= inv; mm_unreal *= inv;
                arb_total *= inv; arb_real *= inv; arb_unreal *= inv; arb_venue *= inv;
                cta_total *= inv; cta_real *= inv; cta_unreal *= inv;
                noise_total *= inv; real_check *= inv;

                const auto median = [](std::vector<double> v) {
                    if (v.empty()) return 0.0;
                    std::sort(v.begin(), v.end());
                    const size_t n = v.size();
                    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
                };
                const double mm_med = median(mm_vals);
                const double arb_med = median(arb_vals);
                const double cta_med = median(cta_vals);
                const double noise_med = median(noise_vals);
                const auto avg = [](const std::vector<double>& v) {
                    if (v.empty()) return 0.0;
                    double s = 0.0;
                    for (double x : v) s += x;
                    return s / static_cast<double>(v.size());
                };
                const double cta_cs_std = avg(cta_cs_std_vals);
                const double cta_pos_frac = avg(cta_pos_frac_vals);
                const double cta_best = avg(cta_best_vals);
                const double cta_worst = avg(cta_worst_vals);

                csv << num(p1) << ',' << num(p2) << ',' << num(p3) << ','
                    << args.n_mm << ',' << args.n_arb << ',' << args.n_cta << ','
                    << args.n_noise << ','
                    << num(mm_total) << ',' << num(mm_real) << ',' << num(mm_unreal) << ','
                    << num(arb_total) << ',' << num(arb_real) << ',' << num(arb_unreal) << ','
                    << num(arb_venue) << ','
                    << num(cta_total) << ',' << num(cta_real) << ',' << num(cta_unreal) << ','
                    << num(noise_total) << ','
                    << num(mm_med) << ',' << num(arb_med) << ','
                    << num(cta_med) << ',' << num(noise_med) << ','
                    << num(real_check) << ','
                    << static_cast<uint64_t>(static_cast<double>(trades) * inv) << ','
                    << static_cast<uint64_t>(static_cast<double>(volume) * inv) << ','
                    << args.latency << ',' << args.seed << ',' << args.repeats << ','
                    << num(args.arb_exit) << ',' << args.arb_hold << ','
                    << args.arb_gap << ',' << (args.arb_net_edge ? 1 : 0) << ','
                    << args.cta_lot << ','
                    << args.cta_lookback << ',' << num(args.cta_exit_bps) << ','
                    << num(args.cta_entry_sigma) << ',' << args.cta_lookback_sigma << ','
                    << num(args.cta_contrarian_frac) << ','
                    << num(cta_cs_std) << ',' << num(cta_pos_frac) << ','
                    << num(cta_best) << ',' << num(cta_worst) << '\n';
                csv.flush();

                ++done;
                std::fprintf(stderr,
                             "[%zu/%zu] p1=%.3f p2=%.3f p3=%.3f  "
                             "mm=%12.2f  arb=%12.2f  cta=%12.2f  real_check=%.3f\n",
                             done, total, p1, p2, p3, mm_total, arb_total,
                             cta_total, real_check);
            }
        }
    }

    std::printf("wrote %s\n", path.c_str());
    return 0;
}
