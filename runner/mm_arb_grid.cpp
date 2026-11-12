// Grid runner for the first MM-Arb model:
//   f(p1, p2) = (total MM PnL, total arbitrageur PnL)
// with N MMs, N arbs and M noise traders, common cash C / base B, delay t.
//
// Every grid point uses the same global seed, so the noise/agent RNG streams
// are common random numbers; only p1 / p2 change.

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

    size_t   n_mm      = 10;
    size_t   n_arb     = 10;
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
    Qty_t mm_lot  = 10;
    Qty_t arb_lot = 10;

    std::string out_dir = "build/mm_arb_grid";
};

void usage(const char* prog) {
    std::printf(
        "usage: %s [--p1 1,2,3] [--p2 0,1,2] [--n-mm N] [--n-arb N]\n"
        "          [--noise N] [--cash C] [--base B] [--latency T]\n"
        "          [--ticks T] [--seed S] [--s1 X] [--s2 X] [--gamma X]\n"
        "          [--mm-lot Q] [--arb-lot Q] [--mm-rest K] [--threads N]\n"
        "          [--repeats R] [--out DIR] [--record]\n",
        prog);
    std::printf(
        "  p1 = mean MM half-spread (ticks), p2 = mean arb edge (ticks).\n"
        "  MM/arb accounts: cash C, base inventory B, position limit 2B.\n"
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
        else if (a == "--n-mm")      args.n_mm = std::stoull(next("--n-mm"));
        else if (a == "--n-arb")     args.n_arb = std::stoull(next("--n-arb"));
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

    if (args.p1s.empty() || args.p2s.empty()) {
        std::fprintf(stderr, "p1/p2 grids must be non-empty\n");
        return 2;
    }
    if (args.repeats == 0) args.repeats = 1;

    std::filesystem::create_directories(args.out_dir);
    const std::string path = args.out_dir + "/f_p1_p2.csv";
    std::ofstream csv(path);
    if (!csv.is_open()) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return 2;
    }

    csv << "p1,p2,n_mm,n_arb,n_noise,"
           "mm_pnl,mm_realized,mm_unrealized,"
           "arb_pnl,arb_realized,arb_unrealized,arb_pnl_venue,"
           "noise_pnl,real_check,trades,volume,latency,seed,repeats\n";

    const size_t total = args.p1s.size() * args.p2s.size();
    size_t done = 0;

    for (double p1 : args.p1s) {
        for (double p2 : args.p2s) {
            double mm_total = 0, mm_real = 0, mm_unreal = 0;
            double arb_total = 0, arb_real = 0, arb_unreal = 0, arb_venue = 0;
            double noise_total = 0, real_check = 0;
            uint64_t trades = 0, volume = 0;

            for (size_t r = 0; r < args.repeats; ++r) {
                av::SimEngine e;
                e.cfg.record   = args.record;
                e.cfg.out_dir  = args.out_dir + "/run_p1_" + tag(p1) +
                                 "_p2_" + tag(p2) + "_s" + std::to_string(args.seed + r);
                e.cfg.ticks    = args.ticks;
                e.cfg.seed     = args.seed + r;
                e.cfg.threads  = args.threads;
                e.cfg.scenario = av::Scenario::MmArb;

                av::MmArbConfig& m = e.cfg.mm_arb;
                m.n_mm           = args.n_mm;
                m.n_arb          = args.n_arb;
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
                m.mm_gamma       = args.gamma;
                m.mm_max_rest_ticks = args.mm_rest;

                e.setup();
                e.run();

                // MM/noise are marked at the real venue mid. The arb is
                // cross-market: the economic PnL marks both legs at the ref
                // fair value; the per-venue PnL marks each leg in its own
                // market. Both are reported.
                const av::SimEngine::PnLSummary mm      = e.pnl_of_class(1);
                const av::SimEngine::PnLSummary arb     = e.pnl_of_class(2);
                const av::SimEngine::PnLSummary arb_econ =
                    e.pnl_of_class(2, -1, e.ref_mark());
                const av::SimEngine::PnLSummary noise   = e.pnl_of_class(0);

                mm_total  += mm.total;    mm_real  += mm.realized;    mm_unreal  += mm.unrealized;
                arb_total += arb_econ.total;
                arb_real  += arb_econ.realized;
                arb_unreal += arb_econ.total - arb_econ.realized;
                arb_venue += arb.total;
                noise_total += noise.total;
                real_check += e.pnl_of_class(1, 0).total +
                              e.pnl_of_class(2, 0).total +
                              e.pnl_of_class(0, 0).total;
                trades += e.real.stats.trade_count;
                volume += e.real.stats.total_volume;
            }

            const double inv = 1.0 / static_cast<double>(args.repeats);
            mm_total *= inv; mm_real *= inv; mm_unreal *= inv;
            arb_total *= inv; arb_real *= inv; arb_unreal *= inv; arb_venue *= inv;
            noise_total *= inv; real_check *= inv;

            csv << num(p1) << ',' << num(p2) << ','
                << args.n_mm << ',' << args.n_arb << ',' << args.n_noise << ','
                << num(mm_total) << ',' << num(mm_real) << ',' << num(mm_unreal) << ','
                << num(arb_total) << ',' << num(arb_real) << ',' << num(arb_unreal) << ','
                << num(arb_venue) << ','
                << num(noise_total) << ',' << num(real_check) << ','
                << static_cast<uint64_t>(static_cast<double>(trades) * inv) << ','
                << static_cast<uint64_t>(static_cast<double>(volume) * inv) << ','
                << args.latency << ',' << args.seed << ',' << args.repeats << '\n';
            csv.flush();

            ++done;
            std::fprintf(stderr,
                         "[%zu/%zu] p1=%.3f p2=%.3f  mm=%12.2f  arb=%12.2f  "
                         "noise=%12.2f  real_check=%.3f\n",
                         done, total, p1, p2, mm_total, arb_total, noise_total,
                         real_check);
        }
    }

    std::printf("wrote %s\n", path.c_str());
    return 0;
}
