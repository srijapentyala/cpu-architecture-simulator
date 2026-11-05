#include "sim/simulator.hpp"

#include <algorithm>
#include <stdexcept>

namespace sim {
namespace {

void bump(uint8_t& counter, bool up) {
    if (up) counter = static_cast<uint8_t>(std::min(3, counter + 1));
    else counter = static_cast<uint8_t>(std::max(0, counter - 1));
}

}  // namespace

// Counters start at 1, weakly not taken, so a first branch costs what static
// fall-through would. Global history is updated when a branch resolves in EX, not at
// fetch, so branches already in flight predict with slightly stale history.
BranchPredictor::BranchPredictor(PredictorKind kind, int tableBits) : kind_(kind) {
    if (tableBits < 1 || tableBits > 24) throw std::runtime_error("predictor table bits must be 1..24");
    const std::size_t entries = std::size_t{1} << tableBits;
    mask_ = entries - 1;
    bimodal_.assign(entries, 1);
    gshare_.assign(entries, 1);
    chooser_.assign(entries, 1);
}

std::size_t BranchPredictor::bimodalIndex(int pc) const { return static_cast<std::size_t>(pc) & mask_; }

std::size_t BranchPredictor::gshareIndex(int pc) const {
    return (static_cast<std::size_t>(pc) ^ static_cast<std::size_t>(history_)) & mask_;
}

bool BranchPredictor::predict(int pc, int target) const {
    switch (kind_) {
        case PredictorKind::AlwaysNotTaken: return false;
        case PredictorKind::Btfn: return target <= pc;
        case PredictorKind::TwoBit: return bimodal_[bimodalIndex(pc)] >= 2;
        case PredictorKind::Gshare: return gshare_[gshareIndex(pc)] >= 2;
        case PredictorKind::Tournament:
            return chooser_[bimodalIndex(pc)] >= 2 ? gshare_[gshareIndex(pc)] >= 2 : bimodal_[bimodalIndex(pc)] >= 2;
    }
    return false;
}

void BranchPredictor::update(int pc, bool taken) {
    if (kind_ == PredictorKind::AlwaysNotTaken || kind_ == PredictorKind::Btfn) return;
    uint8_t& bimodal = bimodal_[bimodalIndex(pc)];
    uint8_t& gshare = gshare_[gshareIndex(pc)];
    if (kind_ == PredictorKind::Tournament) {
        const bool bimodalRight = (bimodal >= 2) == taken;
        const bool gshareRight = (gshare >= 2) == taken;
        // The chooser only learns when the two components disagree on who was right.
        if (bimodalRight != gshareRight) bump(chooser_[bimodalIndex(pc)], gshareRight);
    }
    bump(bimodal, taken);
    bump(gshare, taken);
    history_ = ((history_ << 1) | (taken ? 1u : 0u)) & mask_;
}

}  // namespace sim
