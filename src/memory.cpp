#include "sim/simulator.hpp"

#include <algorithm>
#include <stdexcept>

namespace sim {
namespace {

CacheConfig legacyConfig(int sets, int ways, int blockWords) {
    CacheConfig config;
    config.sets = sets;
    config.ways = ways;
    config.blockWords = blockWords;
    return config;
}

// Instructions and data share the L2, so they need distinct addresses there.
constexpr uint32_t kInstructionBase = 1u << 28;

}  // namespace

Cache::Cache(int sets, int ways, int blockWords) : Cache(legacyConfig(sets, ways, blockWords)) {}

Cache::Cache(const CacheConfig& config) : config_(config) {
    if (config.sets <= 0 || config.ways <= 0 || config.blockWords <= 0) throw std::runtime_error("invalid cache config");
    if (config.hitLatency < 1) throw std::runtime_error("cache hit latency must be at least 1");
    lines_.assign(static_cast<size_t>(config.sets), std::vector<Line>(static_cast<size_t>(config.ways)));
}

bool Cache::access(uint32_t wordAddr) { return lookup(wordAddr, false, true).hit; }

Cache::Result Cache::lookup(uint32_t wordAddr, bool isWrite, bool allocateOnMiss) {
    const auto sets = static_cast<uint32_t>(config_.sets);
    const uint32_t block = wordAddr / static_cast<uint32_t>(config_.blockWords);
    const uint32_t index = block % sets;
    const uint64_t tag = block / sets;
    auto& set = lines_[index];
    ++clock_;

    Result result;
    for (auto& line : set) {
        if (line.valid && line.tag == tag) {
            line.lastUsed = clock_;
            if (isWrite && config_.write == WritePolicy::WriteBack) line.dirty = true;
            ++hits_;
            result.hit = true;
            return result;
        }
    }
    ++misses_;
    if (!allocateOnMiss) return result;

    auto victim = std::find_if(set.begin(), set.end(), [](const Line& line) { return !line.valid; });
    if (victim == set.end()) {
        switch (config_.replacement) {
            case Replacement::Lru:
                victim = std::min_element(set.begin(), set.end(), [](const Line& a, const Line& b) { return a.lastUsed < b.lastUsed; });
                break;
            case Replacement::Fifo:
                victim = std::min_element(set.begin(), set.end(), [](const Line& a, const Line& b) { return a.filledAt < b.filledAt; });
                break;
            case Replacement::Random:
                rng_ ^= rng_ << 13;
                rng_ ^= rng_ >> 7;
                rng_ ^= rng_ << 17;
                victim = set.begin() + static_cast<std::ptrdiff_t>(rng_ % set.size());
                break;
        }
        if (victim->dirty) {
            result.evictedDirty = true;
            const uint64_t victimBlock = victim->tag * sets + index;
            result.victimWord = static_cast<uint32_t>(victimBlock * static_cast<uint64_t>(config_.blockWords));
        }
    }
    victim->valid = true;
    victim->tag = tag;
    victim->dirty = isWrite && config_.write == WritePolicy::WriteBack;
    victim->lastUsed = clock_;
    victim->filledAt = clock_;
    return result;
}

MemoryHierarchy::MemoryHierarchy(const SimConfig& config) : l1d_(config.l1d), memLatency_(config.memLatency) {
    if (config.memLatency < 0) throw std::runtime_error("memory latency cannot be negative");
    if (config.l1i) l1i_ = std::make_unique<Cache>(*config.l1i);
    if (config.l2) l2_ = std::make_unique<Cache>(*config.l2);
}

// Cycles below the L1: the L2 hit time, plus memory on an L2 miss.
int MemoryHierarchy::lower(uint32_t wordAddr) {
    if (!l2_) {
        ++memReads_;
        return memLatency_;
    }
    const auto result = l2_->lookup(wordAddr, false, true);
    if (result.evictedDirty) {
        ++l2Stats_.writebacks;
        ++memWrites_;
    }
    if (result.hit) {
        ++l2Stats_.hits;
        return l2_->config().hitLatency;
    }
    ++l2Stats_.misses;
    ++memReads_;
    return l2_->config().hitLatency + memLatency_;
}

void MemoryHierarchy::postWrite(uint32_t wordAddr) {
    if (!l2_) {
        ++memWrites_;
        return;
    }
    const auto& cfg = l2_->config();
    const auto result = l2_->lookup(wordAddr, true, cfg.writeAllocate);
    if (result.evictedDirty) {
        ++l2Stats_.writebacks;
        ++memWrites_;
    }
    if (result.hit) ++l2Stats_.hits;
    else ++l2Stats_.misses;
    if (!result.hit && cfg.writeAllocate) ++memReads_;
    // A write-through L2, or a miss that does not allocate, passes the write straight on.
    if (cfg.write == WritePolicy::WriteThrough || (!result.hit && !cfg.writeAllocate)) ++memWrites_;
}

void MemoryHierarchy::noteEviction(const Cache::Result& result, LevelStats& stats, bool) {
    if (result.evictedDirty) {
        ++stats.writebacks;
        postWrite(result.victimWord);
    }
}

int MemoryHierarchy::read(Cache& l1, LevelStats& stats, uint32_t wordAddr) {
    const auto result = l1.lookup(wordAddr, false, true);
    if (result.hit) {
        ++stats.hits;
        return l1.config().hitLatency;
    }
    ++stats.misses;
    noteEviction(result, stats, false);
    return l1.config().hitLatency + lower(wordAddr);
}

int MemoryHierarchy::data(uint32_t wordAddr, bool isWrite) {
    if (!isWrite) return read(l1d_, l1dStats_, wordAddr);

    const auto& cfg = l1d_.config();
    const auto result = l1d_.lookup(wordAddr, true, cfg.writeAllocate);
    int latency = cfg.hitLatency;
    if (result.hit) {
        ++l1dStats_.hits;
    } else {
        ++l1dStats_.misses;
        noteEviction(result, l1dStats_, false);
        // A write-allocate miss has to fetch the block before it can be modified.
        if (cfg.writeAllocate) latency += lower(wordAddr);
    }
    if (cfg.write == WritePolicy::WriteThrough || (!result.hit && !cfg.writeAllocate)) postWrite(wordAddr);
    return latency;
}

int MemoryHierarchy::fetch(uint32_t pc) {
    if (!l1i_) return 1;
    return read(*l1i_, l1iStats_, kInstructionBase + pc);
}

void MemoryHierarchy::report(Metrics& metrics) const {
    metrics.cacheHits = l1dStats_.hits;
    metrics.cacheMisses = l1dStats_.misses;
    metrics.memReads = memReads_;
    metrics.memWrites = memWrites_;
    metrics.levels.clear();
    if (l1i_) metrics.levels.push_back(l1iStats_);
    metrics.levels.push_back(l1dStats_);
    if (l2_) metrics.levels.push_back(l2Stats_);
}

}  // namespace sim
