#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace sim {

enum class Op { Add, Addi, Sub, Mul, And, Andi, Or, Xor, Slt, Sll, Slli, Srl, Lw, Sw, Beq, Bne, Blt, Bge, J, Nop };

struct Instr {
    Op op = Op::Nop;
    int rd = 0;
    int rs = 0;
    int rt = 0;
    int imm = 0;
    int pc = 0;
};

const char* opName(Op op);

struct LevelStats {
    std::string name;
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t writebacks = 0;

    double missRate() const {
        const uint64_t total = hits + misses;
        return total == 0 ? 0.0 : static_cast<double>(misses) / static_cast<double>(total);
    }
};

struct Metrics {
    uint64_t cycles = 0;
    uint64_t retired = 0;
    uint64_t cacheHits = 0;    // L1 data cache
    uint64_t cacheMisses = 0;  // L1 data cache
    uint64_t branchCorrect = 0;
    uint64_t branchMispredict = 0;

    // Why cycles were lost. See the accounting identity in the README.
    uint64_t loadUseStalls = 0;
    uint64_t dataHazardStalls = 0;  // only when forwarding is off
    uint64_t flushCycles = 0;
    uint64_t memStallCycles = 0;
    uint64_t fetchStallCycles = 0;

    uint64_t loads = 0;
    uint64_t stores = 0;
    uint64_t memReads = 0;
    uint64_t memWrites = 0;
    std::vector<LevelStats> levels;

    double cpi() const { return retired == 0 ? 0.0 : static_cast<double>(cycles) / static_cast<double>(retired); }
    double missRate() const {
        const uint64_t total = cacheHits + cacheMisses;
        return total == 0 ? 0.0 : static_cast<double>(cacheMisses) / static_cast<double>(total);
    }
    double mispredictRate() const {
        const uint64_t total = branchCorrect + branchMispredict;
        return total == 0 ? 0.0 : static_cast<double>(branchMispredict) / static_cast<double>(total);
    }
};

struct ArchState {
    std::array<int32_t, 32> reg{};
    std::vector<int32_t> mem;
};

enum class PredictorKind { AlwaysNotTaken, Btfn, TwoBit, Gshare, Tournament };
enum class WritePolicy { WriteBack, WriteThrough };
enum class Replacement { Lru, Fifo, Random };

struct CacheConfig {
    int sets = 4;
    int ways = 1;
    int blockWords = 1;
    int hitLatency = 1;
    WritePolicy write = WritePolicy::WriteThrough;
    Replacement replacement = Replacement::Lru;
    bool writeAllocate = true;
};

struct SimConfig {
    bool pipeline = true;
    bool forwarding = true;
    PredictorKind predictor = PredictorKind::TwoBit;
    int predictorBits = 8;  // log2 of each table's entries

    CacheConfig l1d;
    std::optional<CacheConfig> l1i;  // absent: instruction fetch never stalls
    std::optional<CacheConfig> l2;   // shared by L1I and L1D
    int memLatency = 0;              // extra cycles to reach memory past the last cache level

    int memWords = 1024;
    uint64_t maxCycles = 500000000;  // exceeding this throws; a run is never silently cut short
    std::ostream* trace = nullptr;   // one CSV row per pipeline cycle
};

class Cache {
public:
    struct Result {
        bool hit = false;
        bool evictedDirty = false;
        uint32_t victimWord = 0;
    };

    Cache(int sets, int ways, int blockWords);
    explicit Cache(const CacheConfig& config);

    bool access(uint32_t wordAddr);
    Result lookup(uint32_t wordAddr, bool isWrite, bool allocateOnMiss);

    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }
    double missRate() const {
        const uint64_t total = hits_ + misses_;
        return total == 0 ? 0.0 : static_cast<double>(misses_) / static_cast<double>(total);
    }
    const CacheConfig& config() const { return config_; }

private:
    struct Line {
        bool valid = false;
        bool dirty = false;
        uint64_t tag = 0;
        uint64_t lastUsed = 0;
        uint64_t filledAt = 0;
    };
    CacheConfig config_;
    uint64_t clock_ = 0;
    uint64_t hits_ = 0;
    uint64_t misses_ = 0;
    uint64_t rng_ = 0x9E3779B97F4A7C15ULL;
    std::vector<std::vector<Line>> lines_;
};

// L1I and L1D in front of an optional shared L2 in front of memory. The caches are
// blocking, and writes to a lower level are posted to an ideal write buffer: they update
// that level's state and statistics but do not stall the pipeline.
class MemoryHierarchy {
public:
    explicit MemoryHierarchy(const SimConfig& config);

    // Total cycles to complete the access, counting the L1 hit latency.
    int data(uint32_t wordAddr, bool isWrite);
    int fetch(uint32_t pc);
    bool hasInstructionCache() const { return l1i_ != nullptr; }
    void report(Metrics& metrics) const;

private:
    int read(Cache& l1, LevelStats& stats, uint32_t wordAddr);
    int lower(uint32_t wordAddr);
    void postWrite(uint32_t wordAddr);
    void noteEviction(const Cache::Result& result, LevelStats& stats, bool toL2Only);

    std::unique_ptr<Cache> l1i_;
    Cache l1d_;
    std::unique_ptr<Cache> l2_;
    int memLatency_;
    LevelStats l1iStats_{"L1I"};
    LevelStats l1dStats_{"L1D"};
    LevelStats l2Stats_{"L2"};
    uint64_t memReads_ = 0;
    uint64_t memWrites_ = 0;
};

class BranchPredictor {
public:
    BranchPredictor(PredictorKind kind, int tableBits);
    bool predict(int pc, int target) const;
    void update(int pc, bool taken);

private:
    std::size_t bimodalIndex(int pc) const;
    std::size_t gshareIndex(int pc) const;

    PredictorKind kind_;
    std::size_t mask_;
    uint64_t history_ = 0;
    std::vector<uint8_t> bimodal_;
    std::vector<uint8_t> gshare_;
    std::vector<uint8_t> chooser_;  // >= 2 means trust gshare
};

std::vector<Instr> parseProgram(const std::string& text);
ArchState runFunctional(const std::vector<Instr>& program, int memWords = 1024);
Metrics runSimulator(const std::vector<Instr>& program, const SimConfig& config, ArchState* state);

}  // namespace sim
