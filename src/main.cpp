#include "sim/simulator.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

void usage() {
    std::cerr << "usage: sim program.asm [options]\n"
                 "  --pipeline | --no-pipeline     5-stage pipeline (default) or single-cycle\n"
                 "  --forwarding | --no-forwarding\n"
                 "  --predictor NAME               not-taken, btfn, twobit, gshare, tournament\n"
                 "  --predictor-bits N             log2 table size (default 8)\n"
                 "  --sets N --ways N --block N    L1D geometry (block in words)\n"
                 "  --write wb|wt  --no-write-allocate  --replacement lru|fifo|random  L1D policies\n"
                 "  --penalty N                    extra cycles to reach memory (alias --mem-latency)\n"
                 "  --l1i SETS,WAYS,BLOCK          add an instruction cache\n"
                 "  --l2 SETS,WAYS,BLOCK,LATENCY   add a shared write-back L2\n"
                 "  --mem-words N                  data memory size (default 1024)\n"
                 "  --max-cycles N                 fail rather than run past this\n"
                 "  --trace FILE                   per-cycle pipeline CSV\n"
                 "  --json                         print full metrics and registers as JSON\n";
}

std::vector<int> splitInts(const std::string& text) {
    std::vector<int> out;
    std::stringstream stream(text);
    std::string part;
    while (std::getline(stream, part, ',')) out.push_back(std::stoi(part));
    return out;
}

void printJson(const sim::Metrics& m, const sim::ArchState& state) {
    std::cout << "{\"cycles\":" << m.cycles << ",\"retired\":" << m.retired << ",\"cpi\":" << m.cpi()
              << ",\"branches\":" << (m.branchCorrect + m.branchMispredict) << ",\"mispredicts\":" << m.branchMispredict
              << ",\"mispredict_rate\":" << m.mispredictRate() << ",\"load_use_stalls\":" << m.loadUseStalls
              << ",\"data_hazard_stalls\":" << m.dataHazardStalls << ",\"flush_cycles\":" << m.flushCycles
              << ",\"mem_stall_cycles\":" << m.memStallCycles << ",\"fetch_stall_cycles\":" << m.fetchStallCycles
              << ",\"loads\":" << m.loads << ",\"stores\":" << m.stores << ",\"mem_reads\":" << m.memReads
              << ",\"mem_writes\":" << m.memWrites << ",\"levels\":[";
    for (std::size_t i = 0; i < m.levels.size(); ++i) {
        const auto& level = m.levels[i];
        std::cout << (i == 0 ? "" : ",") << "{\"name\":\"" << level.name << "\",\"hits\":" << level.hits
                  << ",\"misses\":" << level.misses << ",\"writebacks\":" << level.writebacks
                  << ",\"miss_rate\":" << level.missRate() << "}";
    }
    std::cout << "],\"regs\":[";
    for (std::size_t i = 0; i < state.reg.size(); ++i) std::cout << (i == 0 ? "" : ",") << state.reg[i];
    std::cout << "]}\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    std::ifstream in(argv[1]);
    if (!in) {
        std::cerr << "cannot open " << argv[1] << "\n";
        return 1;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    sim::SimConfig cfg;
    bool json = false;
    std::ofstream traceFile;
    try {
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            const auto value = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
                return argv[++i];
            };
            if (a == "--pipeline") cfg.pipeline = true;
            else if (a == "--no-pipeline") cfg.pipeline = false;
            else if (a == "--forwarding") cfg.forwarding = true;
            else if (a == "--no-forwarding") cfg.forwarding = false;
            else if (a == "--predictor") {
                const std::string v = value();
                if (v == "not-taken") cfg.predictor = sim::PredictorKind::AlwaysNotTaken;
                else if (v == "btfn") cfg.predictor = sim::PredictorKind::Btfn;
                else if (v == "twobit") cfg.predictor = sim::PredictorKind::TwoBit;
                else if (v == "gshare") cfg.predictor = sim::PredictorKind::Gshare;
                else if (v == "tournament") cfg.predictor = sim::PredictorKind::Tournament;
                else throw std::runtime_error("bad predictor: " + v);
            }
            else if (a == "--predictor-bits") cfg.predictorBits = std::stoi(value());
            else if (a == "--sets") cfg.l1d.sets = std::stoi(value());
            else if (a == "--ways") cfg.l1d.ways = std::stoi(value());
            else if (a == "--block") cfg.l1d.blockWords = std::stoi(value());
            else if (a == "--write") {
                const std::string v = value();
                if (v == "wb") cfg.l1d.write = sim::WritePolicy::WriteBack;
                else if (v == "wt") cfg.l1d.write = sim::WritePolicy::WriteThrough;
                else throw std::runtime_error("bad write policy: " + v);
            }
            else if (a == "--no-write-allocate") cfg.l1d.writeAllocate = false;
            else if (a == "--replacement") {
                const std::string v = value();
                if (v == "lru") cfg.l1d.replacement = sim::Replacement::Lru;
                else if (v == "fifo") cfg.l1d.replacement = sim::Replacement::Fifo;
                else if (v == "random") cfg.l1d.replacement = sim::Replacement::Random;
                else throw std::runtime_error("bad replacement policy: " + v);
            }
            else if (a == "--penalty" || a == "--mem-latency") cfg.memLatency = std::stoi(value());
            else if (a == "--l1i") {
                const auto parts = splitInts(value());
                if (parts.size() != 3) throw std::runtime_error("--l1i wants SETS,WAYS,BLOCK");
                sim::CacheConfig l1i;
                l1i.sets = parts[0];
                l1i.ways = parts[1];
                l1i.blockWords = parts[2];
                cfg.l1i = l1i;
            }
            else if (a == "--l2") {
                const auto parts = splitInts(value());
                if (parts.size() != 4) throw std::runtime_error("--l2 wants SETS,WAYS,BLOCK,LATENCY");
                sim::CacheConfig l2;
                l2.sets = parts[0];
                l2.ways = parts[1];
                l2.blockWords = parts[2];
                l2.hitLatency = parts[3];
                l2.write = sim::WritePolicy::WriteBack;
                cfg.l2 = l2;
            }
            else if (a == "--mem-words") cfg.memWords = std::stoi(value());
            else if (a == "--max-cycles") cfg.maxCycles = std::stoull(value());
            else if (a == "--trace") {
                const std::string path = value();
                traceFile.open(path);
                if (!traceFile) throw std::runtime_error("cannot write trace file " + path);
                cfg.trace = &traceFile;
            }
            else if (a == "--json") json = true;
            else {
                usage();
                throw std::runtime_error("unknown arg " + a);
            }
        }
        const auto prog = sim::parseProgram(buf.str());
        sim::ArchState st;
        const auto m = sim::runSimulator(prog, cfg, &st);
        if (json) {
            printJson(m, st);
        } else {
            std::cout << "cycles=" << m.cycles << " retired=" << m.retired << " cpi=" << m.cpi()
                      << " miss_rate=" << m.missRate() << " mispredicts=" << m.branchMispredict << "\n";
        }
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
    return 0;
}
