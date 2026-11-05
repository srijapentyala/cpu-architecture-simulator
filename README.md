# CPU Architecture Simulator

A cycle-oriented simulator for a small integer ISA, written in C++17. It runs real programs through a 5-stage pipeline with forwarding, pluggable branch predictors, and a configurable cache hierarchy, and it reports where every cycle went. Python tooling runs benchmark matrices and analyzes per-cycle traces.

It models timing, not a real chip. Use it to compare configurations against each other. The numbers do not predict how any actual processor performs.

## Quick start

```bash
cmake -S . -B build
cmake --build build -j
python3 -m venv .venv && .venv/bin/pip install -r requirements-dev.txt
cmake -S . -B build              # re-run so ctest picks up the venv and registers pytest
ctest --test-dir build --output-on-failure

./build/sim programs/bubble_sort.asm --mem-words 65536 --predictor gshare --sets 64 --ways 2 --block 4 --write wb --mem-latency 20
.venv/bin/python scripts/run_benchmarks.py --markdown
```

## What it models

**Pipeline.** IF, ID, EX, MEM, WB, joined by four latches and evaluated back to front each cycle.

| Event | Cost |
|---|---|
| First instruction | retires in cycle 5 |
| ALU result used by the next instruction, with forwarding | 0 |
| Load result used by the next instruction | 1 stall |
| Dependent instruction right behind its producer, no forwarding | 2 stalls |
| Mispredicted conditional branch, resolved in EX | 2 flushed cycles |
| `J`, or a correctly predicted taken branch | 0 (perfect BTB, see limits) |
| Data or instruction cache miss | the whole pipeline freezes for the extra latency |

**Branch predictors** (`--predictor`): `not-taken`, `btfn` (backward taken, forward not), `twobit` (bimodal), `gshare`, and `tournament` (bimodal and gshare with a per-branch chooser). `--predictor-bits` sets the table size.

**Memory.** A configurable L1D with sets, ways, block size, write-back or write-through, write-allocate or not, and LRU, FIFO, or random replacement. Optional L1I and a shared write-back L2. Writes to a lower level go through an ideal write buffer: they update its state and counters but do not stall.

**ISA.** `ADD SUB MUL AND OR XOR SLT SLL SRL`, `ADDI ANDI SLLI`, `LW SW`, `BEQ BNE BLT BGE`, `J`, `NOP`. `r0` is hardwired to zero, addresses are byte addresses and must be word-aligned, labels end in `:`, and `#` starts a comment. Arithmetic wraps at 32 bits.

## Where the cycles go

The simulator accounts for every cycle. For any program that does not end on a mispredicted branch:

```
cycles = retired + 4 + load_use + data_hazard + flush + mem_stall + fetch_stall
```

`scripts/parse_trace.py` rebuilds that breakdown from a per-cycle trace (`sim --trace FILE`). This is `bubble_sort` under the base configuration below:

```text
cycles 285,606  retired 232,173  CPI 1.230
  useful work                    232,173   81.3%
  load-use stalls                 32,641   11.4%
  branch flushes                  19,508    6.8%
  data memory stalls               1,280    0.4%
  pipeline fill                        4    0.0%
```

`--diagram N` charts the first N cycles. Here is a load-use stall from `LW r1, 0(r0)`, `ADD r2, r1, r1`, `NOP`. The `ADD` waits in ID, and nothing new is fetched during the stall:

```text
pc       1   2   3   4   5   6   7   8
0       IF  ID  EX  ME  WB   .   .   .
1        .  IF  ID  ID  EX  ME  WB   .
2        .   .   .  IF  ID  EX  ME  WB
```

## Results

Base configuration: 32 sets, 2 ways, 4-word blocks (256 words total), write-back, bimodal predictor, 20 cycles to memory. Each row changes one thing against it. These numbers come from `scripts/run_benchmarks.py`, are deterministic, and live in [`results/benchmarks.csv`](results/benchmarks.csv), with the full table in [`results/summary.md`](results/summary.md).

| Question | Program | Change | Result |
|---|---|---|---|
| Does global history help? | `branch_patterns` | bimodal → gshare | mispredict rate 25% → 0.02%, CPI 1.207 → 1.000 |
| Does it always help? | `bubble_sort` | bimodal → gshare | CPI 1.230 → 1.233. Data-dependent branches have no pattern to learn |
| What does block size do for sequential access? | `seq_sum` | 1 → 4 → 16 words, same capacity | L1D miss rate 100% → 25% → 6.25%, CPI 5.33 → 2.21 → 1.43 |
| What does an L2 do when locality is poor? | `pointer_chase` | add 256×4×8-word L2, 6 cycles | CPI 5.68 → 2.60, memory reads 51,021 → 512 |
| What is forwarding worth? | `matmul` | forwarding off | CPI 2.23 → 2.90, same answer |
| What does write-back save? | `seq_sum` | write-through → write-back | memory writes 2,048 → 512. CPI is unchanged because writes are buffered in this model |

The programs in `programs/` are written to stress one thing each: sequential and strided access, matrix multiply, data-dependent branches, pointer chasing, and periodic branches.

## How it is tested

```bash
ctest --test-dir build --output-on-failure
```

- **Hand-computed timings.** A pipeline can be functionally right and still be off by a stage, so GoogleTest pins the exact cycle count of small programs for each hazard in the table above.
- **Differential testing.** 300 random programs run under 10 configurations and must reach the exact registers and memory of a separate untimed functional model.
- **Cycle accounting.** The identity above is asserted on the same random programs, so a miscounted stall fails even when the program's answer is right.
- **Independent answers.** `pytest` checks each benchmark's result under all 15 configurations against values computed in plain Python, not by the simulator.
- **Trace agreement.** Totals rebuilt from the per-cycle trace must equal the simulator's own counters.
- **Failure paths.** Unaligned or out-of-range accesses, bad operands, and runaway programs are errors. A run is never silently cut short.

Planting 10 deliberate bugs (dropped forwarding paths, no load-use stall, a wrong-path store, miscounted flushes, ignored dirty evictions, and so on) made the GoogleTest suite fail for all 10. The suites also pass under AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cmake -S . -B build-san -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSIM_SANITIZE=address,undefined
```

## Known limits

- **Perfect BTB.** The target of a predicted-taken branch is known at fetch, so taken branches have no decode bubble. Real front ends have one, and gain from a BTB that this model does not have.
- **Non-speculative history.** Global history updates when a branch resolves in EX, not at fetch, so a branch fetched right behind another predicts with slightly stale history.
- **Blocking caches.** A miss freezes the whole pipeline. There are no MSHRs, no hit-under-miss, and no prefetching.
- **Ideal write buffer.** Writes to lower levels never stall, which is why write policy changes memory traffic but not CPI here.
- **No clock period.** `--no-pipeline` reports CPI near 1, but a single-cycle design needs a much longer clock. Compare CPI only between pipelined configurations.
- **Single-issue, in-order, one core.** There is no superscalar, out-of-order, coherence, or virtual memory model.

## Layout

| Path | What |
|---|---|
| `include/sim/simulator.hpp` | Public types and interfaces |
| `src/simulator.cpp` | Pipeline and functional reference model |
| `src/memory.cpp` | Caches and the hierarchy |
| `src/predictor.cpp` | Branch predictors |
| `src/isa.cpp` | Assembler |
| `src/main.cpp` | Command line |
| `programs/` | Benchmark programs |
| `scripts/run_benchmarks.py` | Runs the program × configuration matrix, writes CSV and Markdown |
| `scripts/parse_trace.py` | Stall attribution, low-IPC windows, pipeline charts |
| `scripts/compare_configs.py` | Quick three-way comparison on `loop.asm` |
| `tests/test_sim.cpp`, `tests/python/` | GoogleTest and pytest suites |
