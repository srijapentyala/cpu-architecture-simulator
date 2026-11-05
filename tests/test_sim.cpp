#include "sim/simulator.hpp"

#include <gtest/gtest.h>

#include <random>
#include <sstream>
#include <string>

namespace {

const char* kProgram = R"(
    ADDI r1, r0, 5
    ADDI r2, r0, 7
    ADD r3, r1, r2
    SUB r8, r2, r1
    SW r3, 0(r0)
    LW r4, 0(r0)
    ADD r5, r4, r1
    BEQ r5, r5, skip
    ADDI r6, r0, 1
skip:
    ADDI r7, r0, 9
)";

sim::Metrics run(const std::string& text, sim::SimConfig cfg = {}, sim::ArchState* state = nullptr) {
    return sim::runSimulator(sim::parseProgram(text), cfg, state);
}

}  // namespace

TEST(Functional, ArithmeticMemoryAndBranch) {
    auto program = sim::parseProgram(kProgram);
    auto state = sim::runFunctional(program);
    EXPECT_EQ(state.reg[3], 12);
    EXPECT_EQ(state.reg[8], 2);
    EXPECT_EQ(state.reg[4], 12);
    EXPECT_EQ(state.reg[5], 17);
    EXPECT_EQ(state.reg[6], 0);
    EXPECT_EQ(state.reg[7], 9);
    EXPECT_EQ(state.mem[0], 12);
}

TEST(Functional, ExtendedAluOperations) {
    auto state = sim::runFunctional(sim::parseProgram(R"(
        ADDI r1, r0, 12
        ADDI r2, r0, 10
        MUL r3, r1, r2
        AND r4, r1, r2
        OR r5, r1, r2
        XOR r6, r1, r2
        SLT r7, r2, r1
        SLT r8, r1, r2
        ADDI r9, r0, 2
        SLL r10, r1, r9
        SRL r11, r1, r9
        ANDI r12, r1, 4
        SLLI r13, r1, 3
    )"));
    EXPECT_EQ(state.reg[3], 120);
    EXPECT_EQ(state.reg[4], 8);
    EXPECT_EQ(state.reg[5], 14);
    EXPECT_EQ(state.reg[6], 6);
    EXPECT_EQ(state.reg[7], 1);
    EXPECT_EQ(state.reg[8], 0);
    EXPECT_EQ(state.reg[10], 48);
    EXPECT_EQ(state.reg[11], 3);
    EXPECT_EQ(state.reg[12], 4);
    EXPECT_EQ(state.reg[13], 96);
}

TEST(Functional, BltAndBgeCompareAsSigned) {
    auto state = sim::runFunctional(sim::parseProgram(R"(
        ADDI r1, r0, -1
        BLT r1, r0, negative
        ADDI r2, r0, 1
negative:
        BGE r0, r1, done
        ADDI r3, r0, 1
done:
        NOP
    )"));
    EXPECT_EQ(state.reg[2], 0);
    EXPECT_EQ(state.reg[3], 0);
}

TEST(Functional, ArithmeticWrapsInsteadOfOverflowing) {
    auto state = sim::runFunctional(sim::parseProgram(R"(
        ADDI r1, r0, 2147483647
        ADDI r2, r1, 1
        MUL r3, r1, r1
    )"));
    EXPECT_EQ(state.reg[2], INT32_MIN);
    EXPECT_EQ(state.reg[3], 1);
}

TEST(Parser, RejectsBadInput) {
    EXPECT_THROW(sim::parseProgram("FROB r1, r2, r3"), std::runtime_error);
    EXPECT_THROW(sim::parseProgram("ADD r1, r2"), std::runtime_error);
    EXPECT_THROW(sim::parseProgram("ADD r1, r2, r99"), std::runtime_error);
    EXPECT_THROW(sim::parseProgram("J nowhere"), std::runtime_error);
    EXPECT_THROW(sim::parseProgram("LW r1, r2"), std::runtime_error);
}

TEST(Memory, RejectsUnalignedAndOutOfRangeAccess) {
    EXPECT_THROW(sim::runFunctional(sim::parseProgram("ADDI r1, r0, 2\nLW r2, 0(r1)\n")), std::runtime_error);
    EXPECT_THROW(sim::runFunctional(sim::parseProgram("ADDI r1, r0, 4096\nLW r2, 0(r1)\n")), std::runtime_error);
    EXPECT_THROW(run("ADDI r1, r0, 2\nNOP\nNOP\nNOP\nLW r2, 0(r1)\n"), std::runtime_error);
}

TEST(Pipeline, MatchesFunctionalModel) {
    auto program = sim::parseProgram(kProgram);
    auto reference = sim::runFunctional(program);
    sim::ArchState state;
    sim::SimConfig cfg;
    auto metrics = sim::runSimulator(program, cfg, &state);
    EXPECT_GT(metrics.retired, 0u);
    EXPECT_EQ(state.reg, reference.reg);
    EXPECT_EQ(state.mem, reference.mem);
}

TEST(Pipeline, RunawayProgramFailsInsteadOfBeingCutShort) {
    sim::SimConfig cfg;
    cfg.maxCycles = 1000;
    EXPECT_THROW(run("spin:\nJ spin\n", cfg), std::runtime_error);
}

// These pin the textbook timings by hand, because a pipeline that is merely functionally
// correct can still be off by a whole stage and every CPI it reports would be wrong.
TEST(Timing, FirstInstructionRetiresInCycleFive) {
    EXPECT_EQ(run("NOP\n").cycles, 5u);
}

TEST(Timing, IndependentInstructionsRetireOnePerCycle) {
    const auto m = run("ADDI r1, r0, 1\nADDI r2, r0, 2\nADDI r3, r0, 3\nADDI r4, r0, 4\n");
    EXPECT_EQ(m.retired, 4u);
    EXPECT_EQ(m.cycles, 8u);
}

TEST(Timing, ForwardedAluDependenceCostsNothing) {
    sim::ArchState state;
    const auto m = run("ADDI r1, r0, 5\nADD r2, r1, r1\nADD r3, r2, r2\n", {}, &state);
    EXPECT_EQ(m.cycles, 7u);
    EXPECT_EQ(state.reg[3], 20);
}

TEST(Timing, LoadUseStallsExactlyOneCycle) {
    const auto m = run("LW r1, 0(r0)\nADD r2, r1, r1\n");
    EXPECT_EQ(m.loadUseStalls, 1u);
    EXPECT_EQ(m.cycles, 7u);
}

TEST(Timing, LoadFollowedByUnrelatedInstructionDoesNotStall) {
    const auto m = run("LW r1, 0(r0)\nADDI r2, r0, 1\nADD r3, r1, r2\n");
    EXPECT_EQ(m.loadUseStalls, 0u);
    EXPECT_EQ(m.cycles, 7u);
}

TEST(Timing, WithoutForwardingAnAdjacentDependenceStallsTwo) {
    sim::SimConfig cfg;
    cfg.forwarding = false;
    sim::ArchState state;
    const auto m = run("ADDI r1, r0, 5\nADD r2, r1, r1\n", cfg, &state);
    EXPECT_EQ(m.dataHazardStalls, 2u);
    EXPECT_EQ(m.cycles, 8u);
    EXPECT_EQ(state.reg[2], 10);
}

TEST(Timing, WithoutForwardingAOneApartDependenceStallsOne) {
    sim::SimConfig cfg;
    cfg.forwarding = false;
    const auto m = run("ADDI r1, r0, 5\nNOP\nADD r2, r1, r1\n", cfg);
    EXPECT_EQ(m.dataHazardStalls, 1u);
}

TEST(Timing, MispredictedBranchCostsTwoCycles) {
    sim::SimConfig cfg;
    cfg.predictor = sim::PredictorKind::AlwaysNotTaken;
    sim::ArchState state;
    const auto m = run("BEQ r0, r0, target\nADDI r1, r0, 1\nADDI r2, r0, 2\ntarget:\nADDI r3, r0, 3\n", cfg, &state);
    EXPECT_EQ(m.retired, 2u);
    EXPECT_EQ(m.branchMispredict, 1u);
    EXPECT_EQ(m.flushCycles, 2u);
    EXPECT_EQ(m.cycles, 8u);
    EXPECT_EQ(state.reg[1], 0);  // wrong path never commits
    EXPECT_EQ(state.reg[2], 0);
    EXPECT_EQ(state.reg[3], 3);
}

TEST(Timing, WrongPathStoreNeverReachesMemory) {
    sim::SimConfig cfg;
    cfg.predictor = sim::PredictorKind::AlwaysNotTaken;
    sim::ArchState state;
    const auto m = run("ADDI r1, r0, 9\nBEQ r0, r0, target\nSW r1, 0(r0)\nSW r1, 4(r0)\ntarget:\nNOP\n", cfg, &state);
    EXPECT_EQ(m.stores, 0u);
    EXPECT_EQ(state.mem[0], 0);
    EXPECT_EQ(state.mem[1], 0);
}

TEST(Timing, JumpHasNoPenalty) {
    const auto m = run("J target\nADDI r1, r0, 1\ntarget:\nNOP\n");
    EXPECT_EQ(m.retired, 2u);
    EXPECT_EQ(m.cycles, 6u);
}

TEST(Timing, MemoryMissFreezesThePipelineForThePenalty) {
    sim::SimConfig cfg;
    cfg.memLatency = 10;
    const auto m = run("LW r1, 0(r0)\nLW r2, 0(r0)\n", cfg);
    EXPECT_EQ(m.cacheMisses, 1u);
    EXPECT_EQ(m.cacheHits, 1u);
    EXPECT_EQ(m.memStallCycles, 10u);
    EXPECT_EQ(m.cycles, 16u);
}

TEST(Timing, SingleCycleModeCountsOnePerInstructionPlusMisses) {
    sim::SimConfig cfg;
    cfg.pipeline = false;
    cfg.memLatency = 4;
    const auto m = run("ADDI r1, r0, 1\nLW r2, 0(r0)\nLW r3, 0(r0)\n", cfg);
    EXPECT_EQ(m.retired, 3u);
    EXPECT_EQ(m.cycles, 7u);
}

TEST(Register, X0StaysZero) {
    auto program = sim::parseProgram("ADDI r0, r0, 5\nADD r0, r0, r0\n");
    auto functional = sim::runFunctional(program);
    EXPECT_EQ(functional.reg[0], 0);
    sim::ArchState state;
    sim::runSimulator(program, {}, &state);
    EXPECT_EQ(state.reg[0], 0);
}

TEST(Register, X0IsNeverForwarded) {
    sim::ArchState state;
    run("ADDI r0, r0, 5\nADD r1, r0, r0\n", {}, &state);
    EXPECT_EQ(state.reg[1], 0);
}

TEST(Predictor, TwoBitMispredictsLessThanAlwaysNotTaken) {
    auto program = sim::parseProgram(R"(
        ADDI r1, r0, 0
        ADDI r2, r0, 8
loop:
        ADDI r1, r1, 1
        BNE r1, r2, loop
    )");
    sim::SimConfig always;
    always.predictor = sim::PredictorKind::AlwaysNotTaken;
    sim::SimConfig twoBit;
    twoBit.predictor = sim::PredictorKind::TwoBit;
    auto a = sim::runSimulator(program, always, nullptr);
    auto b = sim::runSimulator(program, twoBit, nullptr);
    sim::ArchState state;
    sim::runSimulator(program, twoBit, &state);
    EXPECT_EQ(state.reg[1], 8);
    EXPECT_LT(b.branchMispredict, a.branchMispredict);
}

TEST(Predictor, BtfnPredictsBackwardTakenAndForwardNot) {
    sim::BranchPredictor btfn(sim::PredictorKind::Btfn, 8);
    EXPECT_TRUE(btfn.predict(10, 2));
    EXPECT_FALSE(btfn.predict(10, 20));
}

TEST(Predictor, TwoBitCounterNeedsTwoOutcomesToFlip) {
    sim::BranchPredictor p(sim::PredictorKind::TwoBit, 8);
    EXPECT_FALSE(p.predict(4, 0));
    p.update(4, true);
    EXPECT_TRUE(p.predict(4, 0));
    p.update(4, true);
    p.update(4, false);
    EXPECT_TRUE(p.predict(4, 0));  // hysteresis: one not-taken does not flip a strong counter
    p.update(4, false);
    EXPECT_FALSE(p.predict(4, 0));
}

namespace {
double accuracyOn(sim::PredictorKind kind, const std::vector<bool>& pattern, int rounds) {
    sim::BranchPredictor p(kind, 10);
    int correct = 0;
    int total = 0;
    for (int r = 0; r < rounds; ++r) {
        for (bool taken : pattern) {
            if (r >= rounds / 2) {
                correct += p.predict(100, 0) == taken ? 1 : 0;
                ++total;
            }
            p.update(100, taken);
        }
    }
    return static_cast<double>(correct) / total;
}
}  // namespace

TEST(Predictor, GshareLearnsAnAlternatingBranchThatDefeatsBimodal) {
    const std::vector<bool> alternating{true, false};
    EXPECT_LE(accuracyOn(sim::PredictorKind::TwoBit, alternating, 1000), 0.55);
    EXPECT_GE(accuracyOn(sim::PredictorKind::Gshare, alternating, 1000), 0.99);
}

TEST(Predictor, TournamentConvergesOnTheBetterComponent) {
    const std::vector<bool> alternating{true, false};
    EXPECT_GE(accuracyOn(sim::PredictorKind::Tournament, alternating, 1000), 0.95);
    const std::vector<bool> biased{true, true, true, true, true, true, true, false};
    EXPECT_GE(accuracyOn(sim::PredictorKind::Tournament, biased, 1000),
              accuracyOn(sim::PredictorKind::TwoBit, biased, 1000) - 0.02);
}

TEST(Cache, StridedSetMissesMoreThanReuse) {
    sim::Cache reuse(4, 1, 1);
    for (int i = 0; i < 4; ++i) reuse.access(static_cast<uint32_t>(i));
    for (int i = 0; i < 4; ++i) reuse.access(static_cast<uint32_t>(i));
    sim::Cache stride(4, 1, 1);
    for (int rep = 0; rep < 2; ++rep) {
        for (uint32_t word : {0u, 4u, 8u, 12u}) stride.access(word);
    }
    EXPECT_GT(stride.missRate(), reuse.missRate());
}

TEST(Cache, LargerBlockGetsSpatialHits) {
    sim::Cache cache(4, 1, 4);
    EXPECT_FALSE(cache.access(0));
    EXPECT_TRUE(cache.access(1));
    EXPECT_TRUE(cache.access(3));
    EXPECT_FALSE(cache.access(4));
}

TEST(Cache, AssociativityRemovesAConflict) {
    sim::Cache direct(4, 1, 1);
    sim::Cache twoWay(2, 2, 1);
    for (int rep = 0; rep < 4; ++rep) {
        for (uint32_t word : {0u, 4u}) {
            direct.access(word);
            twoWay.access(word);
        }
    }
    EXPECT_EQ(direct.hits(), 0u);
    EXPECT_EQ(twoWay.hits(), 6u);
}

TEST(Cache, LruAndFifoEvictDifferently) {
    sim::CacheConfig cfg;
    cfg.sets = 1;
    cfg.ways = 2;
    cfg.replacement = sim::Replacement::Lru;
    sim::Cache lru(cfg);
    cfg.replacement = sim::Replacement::Fifo;
    sim::Cache fifo(cfg);
    for (uint32_t word : {0u, 1u, 0u, 2u}) {  // touching 0 again protects it only under LRU
        lru.access(word);
        fifo.access(word);
    }
    EXPECT_TRUE(lru.access(0));
    EXPECT_FALSE(fifo.access(0));
}

TEST(Cache, WriteBackReportsTheDirtyVictimsAddress) {
    sim::CacheConfig cfg;
    cfg.sets = 2;
    cfg.ways = 1;
    cfg.blockWords = 4;
    cfg.write = sim::WritePolicy::WriteBack;
    sim::Cache cache(cfg);
    EXPECT_FALSE(cache.lookup(13, true, true).evictedDirty);
    const auto second = cache.lookup(21, false, true);  // same set as block 3
    EXPECT_FALSE(second.hit);
    EXPECT_TRUE(second.evictedDirty);
    EXPECT_EQ(second.victimWord, 12u);  // start of the block holding word 13
}

TEST(Hierarchy, WriteThroughSendsEveryWriteToMemoryAndWriteBackOnlyEvictions) {
    const std::string program = R"(
        ADDI r1, r0, 0
        ADDI r2, r0, 100
loop:
        SW r1, 0(r0)
        ADDI r1, r1, 1
        BNE r1, r2, loop
        NOP
    )";
    sim::SimConfig through;
    through.l1d.write = sim::WritePolicy::WriteThrough;
    EXPECT_EQ(run(program, through).memWrites, 100u);
    sim::SimConfig back;
    back.l1d.write = sim::WritePolicy::WriteBack;
    EXPECT_EQ(run(program, back).memWrites, 0u);  // the one block stays dirty in the L1
}

TEST(Hierarchy, DirtyEvictionCountsAWriteback) {
    sim::SimConfig cfg;
    cfg.l1d.sets = 1;
    cfg.l1d.ways = 1;
    cfg.l1d.write = sim::WritePolicy::WriteBack;
    const auto m = run("SW r0, 0(r0)\nLW r1, 4(r0)\n", cfg);
    ASSERT_EQ(m.levels.size(), 1u);
    EXPECT_EQ(m.levels[0].writebacks, 1u);
    EXPECT_EQ(m.memWrites, 1u);
}

TEST(Hierarchy, NoWriteAllocateLeavesTheLineOut) {
    sim::SimConfig cfg;
    cfg.l1d.writeAllocate = false;
    const auto m = run("SW r0, 0(r0)\nLW r1, 0(r0)\n", cfg);
    EXPECT_EQ(m.cacheMisses, 2u);  // the store did not bring the block in
    EXPECT_EQ(m.memWrites, 1u);
}

TEST(Hierarchy, L2HitIsCheaperThanMemory) {
    sim::SimConfig cfg;
    cfg.l1d.sets = 1;
    cfg.l1d.ways = 1;
    cfg.memLatency = 100;
    sim::CacheConfig l2;
    l2.sets = 64;
    l2.ways = 4;
    l2.hitLatency = 10;
    l2.write = sim::WritePolicy::WriteBack;
    cfg.l2 = l2;
    // A misses both levels, B misses both, and A misses L1 but hits L2.
    const auto m = run("LW r1, 0(r0)\nLW r2, 4(r0)\nLW r3, 0(r0)\n", cfg);
    EXPECT_EQ(m.memStallCycles, 110u + 110u + 10u);
    EXPECT_EQ(m.memReads, 2u);
    ASSERT_EQ(m.levels.size(), 2u);
    EXPECT_EQ(m.levels[1].name, "L2");
    EXPECT_EQ(m.levels[1].hits, 1u);
    EXPECT_EQ(m.levels[1].misses, 2u);
}

TEST(Hierarchy, InstructionCacheMissesStallFetch) {
    sim::SimConfig cfg;
    sim::CacheConfig l1i;
    l1i.sets = 4;
    l1i.ways = 1;
    l1i.blockWords = 2;
    cfg.l1i = l1i;
    cfg.memLatency = 5;
    const auto m = run("NOP\nNOP\nNOP\nNOP\n", cfg);
    ASSERT_EQ(m.levels[0].name, "L1I");
    EXPECT_EQ(m.levels[0].misses, 2u);
    EXPECT_EQ(m.levels[0].hits, 2u);
    EXPECT_EQ(m.fetchStallCycles, 10u);
    EXPECT_EQ(m.cycles, 8u + 10u);
}

TEST(Hierarchy, InvalidGeometryIsRejected) {
    sim::SimConfig cfg;
    cfg.l1d.ways = 0;
    EXPECT_THROW(run("NOP\n", cfg), std::runtime_error);
}

TEST(Trace, HasOneRowPerCycleThatSumsToTheMetrics) {
    std::ostringstream trace;
    sim::SimConfig cfg;
    cfg.trace = &trace;
    const auto m = run("LW r1, 0(r0)\nADD r2, r1, r1\n", cfg);
    std::istringstream lines(trace.str());
    std::string line;
    std::getline(lines, line);
    EXPECT_EQ(line, "cycle,if,id,ex,mem,wb,load_use,data_stall,flush,mem_stall,fetch_stall,retired");
    uint64_t rows = 0;
    while (std::getline(lines, line)) ++rows;
    EXPECT_EQ(rows, m.cycles);
}

namespace {

// Random programs with no way to loop forever: every branch goes forward, except one
// outer loop that counts down a register nothing else touches. Addresses stay in range
// because the base register is masked to a word-aligned value first.
std::string randomProgram(std::mt19937& rng, int length) {
    std::ostringstream out;
    out << "ADDI r31, r0, " << (2 + rng() % 4) << "\nouter:\n";
    const auto reg = [&] { return 1 + rng() % 12; };
    for (int i = 0; i < length; ++i) {
        out << "L" << i << ":\n";
        const unsigned rd = reg();
        const unsigned rs = reg();
        const unsigned rt = reg();
        switch (rng() % 14) {
            case 0: out << "ADD r" << rd << ", r" << rs << ", r" << rt << "\n"; break;
            case 1: out << "SUB r" << rd << ", r" << rs << ", r" << rt << "\n"; break;
            case 2: out << "ADDI r" << rd << ", r" << rs << ", " << static_cast<int>(rng() % 200) - 100 << "\n"; break;
            case 3: out << "MUL r" << rd << ", r" << rs << ", r" << rt << "\n"; break;
            case 4: out << "XOR r" << rd << ", r" << rs << ", r" << rt << "\n"; break;
            case 5: out << "SLT r" << rd << ", r" << rs << ", r" << rt << "\n"; break;
            case 6: out << "SLL r" << rd << ", r" << rs << ", r" << rt << "\n"; break;
            case 7: out << "SRL r" << rd << ", r" << rs << ", r" << rt << "\n"; break;
            case 8:
            case 9:
                out << "ANDI r13, r" << rs << ", 252\nLW r" << rd << ", " << 4 * (rng() % 8) << "(r13)\n";
                break;
            case 10:
                out << "ANDI r13, r" << rs << ", 252\nSW r" << rt << ", " << 4 * (rng() % 8) << "(r13)\n";
                break;
            case 11:
            case 12: {
                const char* names[] = {"BEQ", "BNE", "BLT", "BGE"};
                const int target = i + 1 + static_cast<int>(rng() % 4);
                out << names[rng() % 4] << " r" << rs << ", r" << rt << ", L" << std::min(target, length) << "\n";
                break;
            }
            default: out << "NOP\n"; break;
        }
    }
    out << "L" << length << ":\nADDI r31, r31, -1\nBNE r31, r0, outer\nNOP\n";
    return out.str();
}

std::vector<sim::SimConfig> interestingConfigs() {
    std::vector<sim::SimConfig> configs;
    for (auto predictor : {sim::PredictorKind::AlwaysNotTaken, sim::PredictorKind::Btfn, sim::PredictorKind::TwoBit,
                           sim::PredictorKind::Gshare, sim::PredictorKind::Tournament}) {
        for (bool forwarding : {true, false}) {
            sim::SimConfig cfg;
            cfg.predictor = predictor;
            cfg.forwarding = forwarding;
            cfg.l1d.sets = 4;
            cfg.l1d.ways = 2;
            cfg.l1d.blockWords = 2;
            cfg.l1d.write = forwarding ? sim::WritePolicy::WriteBack : sim::WritePolicy::WriteThrough;
            cfg.memLatency = 7;
            sim::CacheConfig l2;
            l2.sets = 16;
            l2.ways = 2;
            l2.hitLatency = 3;
            l2.write = sim::WritePolicy::WriteBack;
            if (predictor == sim::PredictorKind::Gshare) cfg.l2 = l2;
            sim::CacheConfig l1i;
            l1i.sets = 8;
            l1i.blockWords = 2;
            if (predictor == sim::PredictorKind::Tournament) cfg.l1i = l1i;
            configs.push_back(cfg);
        }
    }
    return configs;
}

}  // namespace

// The strongest correctness check here: on random programs the pipeline must reach the
// reference model's exact state under every predictor and with forwarding on or off.
TEST(Differential, PipelineMatchesFunctionalOnRandomPrograms) {
    std::mt19937 rng(12345);
    const auto configs = interestingConfigs();
    for (int trial = 0; trial < 300; ++trial) {
        const auto text = randomProgram(rng, 40);
        const auto program = sim::parseProgram(text);
        const auto reference = sim::runFunctional(program);
        for (const auto& cfg : configs) {
            sim::ArchState state;
            const auto m = sim::runSimulator(program, cfg, &state);
            ASSERT_EQ(state.reg, reference.reg) << "trial " << trial << "\n" << text;
            ASSERT_EQ(state.mem, reference.mem) << "trial " << trial << "\n" << text;
            ASSERT_GT(m.retired, 0u);
        }
    }
}

// Every cycle has to be explained: an ideal pipeline's retired + 4 startup cycles, plus
// the stall causes the simulator claims. If any cause were miscounted, this would break.
// The programs end in a NOP, so a final mispredicted branch does not change the total.
TEST(Differential, CyclesAreFullyExplainedByTheStallCounters) {
    std::mt19937 rng(777);
    const auto configs = interestingConfigs();
    for (int trial = 0; trial < 300; ++trial) {
        const auto text = randomProgram(rng, 40);
        const auto program = sim::parseProgram(text);
        for (const auto& cfg : configs) {
            const auto m = sim::runSimulator(program, cfg, nullptr);
            const uint64_t explained = m.retired + 4 + m.loadUseStalls + m.dataHazardStalls + m.flushCycles +
                                       m.memStallCycles + m.fetchStallCycles;
            ASSERT_EQ(m.cycles, explained) << "trial " << trial << "\n" << text;
        }
    }
}

// The same instructions retire in the same order whatever the timing, so the predictor
// and hazard settings can only move the cycle count, never the retired count.
TEST(Differential, RetiredCountDoesNotDependOnTiming) {
    std::mt19937 rng(99);
    for (int trial = 0; trial < 100; ++trial) {
        const auto program = sim::parseProgram(randomProgram(rng, 30));
        sim::SimConfig single;
        single.pipeline = false;
        const auto baseline = sim::runSimulator(program, single, nullptr);
        for (const auto& cfg : interestingConfigs()) {
            ASSERT_EQ(sim::runSimulator(program, cfg, nullptr).retired, baseline.retired);
        }
    }
}
