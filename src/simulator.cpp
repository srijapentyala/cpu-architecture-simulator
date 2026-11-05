#include "sim/simulator.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace sim {
namespace {

bool writesReg(const Instr& in) {
    switch (in.op) {
        case Op::Sw:
        case Op::Beq:
        case Op::Bne:
        case Op::Blt:
        case Op::Bge:
        case Op::J:
        case Op::Nop:
            return false;
        default:
            return true;
    }
}
bool readsRs(const Instr& in) { return in.op != Op::J && in.op != Op::Nop; }
bool readsRt(const Instr& in) {
    switch (in.op) {
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::And:
        case Op::Or:
        case Op::Xor:
        case Op::Slt:
        case Op::Sll:
        case Op::Srl:
        case Op::Sw:
        case Op::Beq:
        case Op::Bne:
        case Op::Blt:
        case Op::Bge:
            return true;
        default:
            return false;
    }
}
bool isCondBranch(const Instr& in) {
    return in.op == Op::Beq || in.op == Op::Bne || in.op == Op::Blt || in.op == Op::Bge;
}
bool isLoad(const Instr& in) { return in.op == Op::Lw; }
bool isStore(const Instr& in) { return in.op == Op::Sw; }

bool usesReg(const Instr& in, int reg) {
    if (reg == 0) return false;
    return (readsRs(in) && in.rs == reg) || (readsRt(in) && in.rt == reg);
}

int wordIndex(int32_t addr, int memWords) {
    if (addr % 4 != 0) throw std::runtime_error("unaligned memory access: " + std::to_string(addr));
    const int idx = addr / 4;
    if (idx < 0 || idx >= memWords) throw std::runtime_error("memory address out of range: " + std::to_string(addr));
    return idx;
}

// Arithmetic wraps like the hardware. Going through uint32_t keeps it defined in C++.
int32_t wrap(uint32_t value) { return static_cast<int32_t>(value); }

int32_t alu(const Instr& in, int32_t a, int32_t b) {
    const auto ua = static_cast<uint32_t>(a);
    const auto ub = static_cast<uint32_t>(b);
    const auto uimm = static_cast<uint32_t>(in.imm);
    switch (in.op) {
        case Op::Add: return wrap(ua + ub);
        case Op::Addi:
        case Op::Lw:
        case Op::Sw: return wrap(ua + uimm);
        case Op::Sub: return wrap(ua - ub);
        case Op::Mul: return wrap(ua * ub);
        case Op::And: return wrap(ua & ub);
        case Op::Andi: return wrap(ua & uimm);
        case Op::Or: return wrap(ua | ub);
        case Op::Xor: return wrap(ua ^ ub);
        case Op::Slt: return a < b ? 1 : 0;
        case Op::Sll: return wrap(ua << (ub & 31));
        case Op::Slli: return wrap(ua << (uimm & 31));
        case Op::Srl: return wrap(ua >> (ub & 31));
        default: return 0;
    }
}

bool branchTaken(const Instr& in, int32_t a, int32_t b) {
    switch (in.op) {
        case Op::Beq: return a == b;
        case Op::Bne: return a != b;
        case Op::Blt: return a < b;
        case Op::Bge: return a >= b;
        default: return false;
    }
}

// The reference model: one instruction at a time, with no timing at all. The pipeline
// has to end in exactly the architectural state this produces.
void execFunctional(ArchState& s, const std::vector<Instr>& program, int memWords, MemoryHierarchy* memory, uint64_t maxSteps, Metrics* metrics) {
    int pc = 0;
    uint64_t steps = 0;
    while (pc >= 0 && pc < static_cast<int>(program.size())) {
        if (++steps > maxSteps) throw std::runtime_error("functional run exceeded step limit");
        const Instr& in = program[static_cast<size_t>(pc)];
        int next = pc + 1;
        int latency = 1;
        const int32_t a = s.reg[in.rs];
        const int32_t b = s.reg[in.rt];
        if (isLoad(in)) {
            const int idx = wordIndex(alu(in, a, b), memWords);
            s.reg[in.rd] = s.mem[static_cast<size_t>(idx)];
            if (memory) latency = memory->data(static_cast<uint32_t>(idx), false);
            if (metrics) ++metrics->loads;
        } else if (isStore(in)) {
            const int idx = wordIndex(alu(in, a, b), memWords);
            s.mem[static_cast<size_t>(idx)] = b;
            if (memory) latency = memory->data(static_cast<uint32_t>(idx), true);
            if (metrics) ++metrics->stores;
        } else if (isCondBranch(in)) {
            if (branchTaken(in, a, b)) next = in.imm;
        } else if (in.op == Op::J) {
            next = in.imm;
        } else if (in.op != Op::Nop) {
            s.reg[in.rd] = alu(in, a, b);
        }
        s.reg[0] = 0;
        if (metrics) {
            ++metrics->retired;
            metrics->cycles += static_cast<uint64_t>(latency);
            metrics->memStallCycles += static_cast<uint64_t>(latency - 1);
        }
        pc = next;
    }
}

struct Latch {
    bool valid = false;
    Instr in{};
    int32_t a = 0;
    int32_t b = 0;
    int32_t result = 0;
    int32_t loaded = 0;
    int predictedNext = 0;
};

std::string cell(const Latch& latch) { return latch.valid ? std::to_string(latch.in.pc) : "-"; }

}  // namespace

ArchState runFunctional(const std::vector<Instr>& program, int memWords) {
    ArchState s;
    s.mem.assign(static_cast<size_t>(memWords), 0);
    execFunctional(s, program, memWords, nullptr, 500000000, nullptr);
    return s;
}

// Five stages joined by four latches, evaluated back to front each cycle so that every
// stage sees the values its predecessor latched last cycle. The textbook timings follow:
// the first instruction retires in cycle 5, a load-use dependence costs one cycle, and a
// branch resolved in EX costs two.
Metrics runSimulator(const std::vector<Instr>& program, const SimConfig& config, ArchState* state) {
    Metrics metrics;
    MemoryHierarchy memory(config);
    ArchState arch;
    arch.mem.assign(static_cast<size_t>(config.memWords), 0);

    if (!config.pipeline) {
        execFunctional(arch, program, config.memWords, &memory, config.maxCycles, &metrics);
        memory.report(metrics);
        if (state) *state = arch;
        return metrics;
    }

    BranchPredictor predictor(config.predictor, config.predictorBits);
    Latch ifId, idEx, exMem, memWb;
    int pc = 0;
    const int programSize = static_cast<int>(program.size());
    if (config.trace) *config.trace << "cycle,if,id,ex,mem,wb,load_use,data_stall,flush,mem_stall,fetch_stall,retired\n";

    while (ifId.valid || idEx.valid || exMem.valid || memWb.valid || (pc >= 0 && pc < programSize)) {
        if (++metrics.cycles > config.maxCycles) throw std::runtime_error("run exceeded maxCycles");
        const uint64_t thisCycle = metrics.cycles;
        const Latch wbStage = memWb;

        // WB writes in the first half of the cycle, so ID below reads the new value.
        bool retiredNow = false;
        if (memWb.valid) {
            if (writesReg(memWb.in) && memWb.in.rd != 0) {
                arch.reg[memWb.in.rd] = isLoad(memWb.in) ? memWb.loaded : memWb.result;
            }
            ++metrics.retired;
            retiredNow = true;
        }

        // MEM. The caches are blocking: a slow access freezes every stage.
        Latch nextMemWb = exMem;
        int memExtra = 0;
        if (exMem.valid && (isLoad(exMem.in) || isStore(exMem.in))) {
            const int idx = wordIndex(exMem.result, config.memWords);
            const int latency = memory.data(static_cast<uint32_t>(idx), isStore(exMem.in));
            memExtra = latency - 1;
            if (isLoad(exMem.in)) {
                nextMemWb.loaded = arch.mem[static_cast<size_t>(idx)];
                ++metrics.loads;
            } else {
                arch.mem[static_cast<size_t>(idx)] = exMem.b;
                ++metrics.stores;
            }
        }

        // EX. Operands were read in ID, but a newer value may be in a later stage. The
        // younger producer (in MEM) takes priority over the older one (in WB).
        Latch nextExMem = idEx;
        bool squash = false;
        int redirect = 0;
        if (idEx.valid) {
            const auto forward = [&](int reg, int32_t fromRegFile) -> int32_t {
                if (reg == 0 || !config.forwarding) return fromRegFile;
                if (exMem.valid && writesReg(exMem.in) && exMem.in.rd == reg && !isLoad(exMem.in)) return exMem.result;
                if (memWb.valid && writesReg(memWb.in) && memWb.in.rd == reg) {
                    return isLoad(memWb.in) ? memWb.loaded : memWb.result;
                }
                return fromRegFile;
            };
            nextExMem.a = readsRs(idEx.in) ? forward(idEx.in.rs, idEx.a) : 0;
            nextExMem.b = readsRt(idEx.in) ? forward(idEx.in.rt, idEx.b) : 0;
            nextExMem.result = alu(idEx.in, nextExMem.a, nextExMem.b);
            if (isCondBranch(idEx.in)) {
                const bool taken = branchTaken(idEx.in, nextExMem.a, nextExMem.b);
                const int actual = taken ? idEx.in.imm : idEx.in.pc + 1;
                predictor.update(idEx.in.pc, taken);
                if (actual != idEx.predictedNext) {
                    squash = true;
                    redirect = actual;
                    ++metrics.branchMispredict;
                    metrics.flushCycles += 2;
                } else {
                    ++metrics.branchCorrect;
                }
            }
        }

        // ID. Hazards are checked against the stages as they stand this cycle.
        bool loadUse = false;
        bool dataStall = false;
        if (ifId.valid && !squash) {
            const Instr& decoding = ifId.in;
            if (config.forwarding) {
                loadUse = idEx.valid && isLoad(idEx.in) && usesReg(decoding, idEx.in.rd);
            } else {
                dataStall = (idEx.valid && writesReg(idEx.in) && usesReg(decoding, idEx.in.rd)) ||
                            (exMem.valid && writesReg(exMem.in) && usesReg(decoding, exMem.in.rd));
            }
        }
        if (loadUse) ++metrics.loadUseStalls;
        if (dataStall) ++metrics.dataHazardStalls;
        const bool stall = loadUse || dataStall;

        Latch nextIdEx;
        if (ifId.valid && !stall && !squash) {
            nextIdEx.valid = true;
            nextIdEx.in = ifId.in;
            nextIdEx.predictedNext = ifId.predictedNext;
            nextIdEx.a = readsRs(ifId.in) ? arch.reg[ifId.in.rs] : 0;
            nextIdEx.b = readsRt(ifId.in) ? arch.reg[ifId.in.rt] : 0;
        }

        // IF. The predictor is consulted with the target already known, so this models
        // a perfect BTB: a taken branch or a J has no decode bubble.
        Latch nextIfId;
        Latch fetched;
        int fetchExtra = 0;
        int nextPc = pc;
        if (squash) {
            nextPc = redirect;
        } else if (stall) {
            nextIfId = ifId;
        } else if (pc >= 0 && pc < programSize) {
            fetched.valid = true;
            fetched.in = program[static_cast<size_t>(pc)];
            fetched.in.pc = pc;
            fetched.predictedNext = pc + 1;
            if (isCondBranch(fetched.in)) {
                if (predictor.predict(pc, fetched.in.imm)) fetched.predictedNext = fetched.in.imm;
            } else if (fetched.in.op == Op::J) {
                fetched.predictedNext = fetched.in.imm;
            }
            fetchExtra = memory.fetch(static_cast<uint32_t>(pc)) - 1;
            nextPc = fetched.predictedNext;
            nextIfId = fetched;
        }

        // An instruction fetch miss and a data miss overlap; the longer one sets the freeze.
        const int freeze = std::max(memExtra, fetchExtra);
        metrics.cycles += static_cast<uint64_t>(freeze);
        const int chargedToFetch = std::max(0, fetchExtra - memExtra);
        metrics.memStallCycles += static_cast<uint64_t>(memExtra);
        metrics.fetchStallCycles += static_cast<uint64_t>(chargedToFetch);

        if (config.trace) {
            *config.trace << thisCycle << ',' << cell(fetched) << ',' << cell(ifId) << ',' << cell(idEx) << ','
                          << cell(exMem) << ',' << cell(wbStage) << ',' << (loadUse ? 1 : 0) << ',' << (dataStall ? 1 : 0)
                          << ',' << (squash ? 2 : 0) << ',' << memExtra << ',' << chargedToFetch << ','
                          << (retiredNow ? 1 : 0) << '\n';
        }

        memWb = nextMemWb;
        exMem = nextExMem;
        idEx = nextIdEx;
        ifId = nextIfId;
        pc = nextPc;
    }

    memory.report(metrics);
    arch.reg[0] = 0;
    if (state) *state = arch;
    return metrics;
}

}  // namespace sim
