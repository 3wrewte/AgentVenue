#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "core/phase.hpp"
#include "runner/sim_engine.hpp"

namespace {

void usage(const char* prog) {
    std::printf(
        "usage: %s [--ticks N] [--agents N] [--seed N] [--delay N]\n"
        "          [--threads N] [--out DIR] [--no-record]\n"
        "  --agents N: noise traders (3 sample strategies are always added)\n",
        prog);
}

} // namespace

int main(int argc, char** argv) {
    av::SimConfig cfg;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(2);
            }
            return argv[++i];
        };

        if (a == "--ticks") {
            cfg.ticks = std::stoull(next("--ticks"));
        } else if (a == "--agents") {
            cfg.noise_agents = static_cast<size_t>(std::stoull(next("--agents")));
        } else if (a == "--seed") {
            cfg.seed = std::stoull(next("--seed"));
        } else if (a == "--delay") {
            cfg.market_delay = static_cast<uint32_t>(std::stoul(next("--delay")));
        } else if (a == "--threads") {
            cfg.threads = static_cast<size_t>(std::stoul(next("--threads")));
        } else if (a == "--out") {
            cfg.out_dir = next("--out");
        } else if (a == "--no-record") {
            cfg.record = false;
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    if (cfg.market_delay < 1) {
        std::fprintf(stderr, "market delay must be >= 1\n");
        return 2;
    }
    if (cfg.noise_agents == 0) {
        std::fprintf(stderr, "agent count must be > 0\n");
        return 2;
    }

    av::SimEngine engine;
    engine.cfg = cfg;
    engine.setup();
    engine.run();
    engine.print_summary();
    return 0;
}
