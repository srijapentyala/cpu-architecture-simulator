#!/usr/bin/env python3
"""Analyze a per-cycle trace written by `sim --trace FILE`.

    python3 scripts/parse_trace.py trace.csv
    python3 scripts/parse_trace.py trace.csv --diagram 20

Prints where the cycles went and the windows with the worst IPC. --diagram draws the
first N cycles as a pipeline chart.
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Iterable, List, Optional


@dataclass
class TraceRow:
    cycle: int
    stages: List[Optional[int]]  # program counters in IF, ID, EX, MEM, WB
    load_use: int
    data_stall: int
    flush: int
    mem_stall: int
    fetch_stall: int
    retired: int


@dataclass
class TraceSummary:
    pipeline_rows: int = 0
    cycles: int = 0
    retired: int = 0
    load_use_stalls: int = 0
    data_hazard_stalls: int = 0
    flush_cycles: int = 0
    mem_stall_cycles: int = 0
    fetch_stall_cycles: int = 0
    startup_cycles: int = 0
    unexplained_cycles: int = 0
    worst_windows: list = field(default_factory=list)

    @property
    def cpi(self) -> float:
        return self.cycles / self.retired if self.retired else 0.0


def read_trace(path: Path) -> List[TraceRow]:
    with open(path, newline="") as handle:
        reader = csv.DictReader(handle)
        expected = ["cycle", "if", "id", "ex", "mem", "wb", "load_use", "data_stall", "flush",
                    "mem_stall", "fetch_stall", "retired"]
        if reader.fieldnames != expected:
            raise ValueError(f"{path}: not a simulator trace (columns: {reader.fieldnames})")
        return [
            TraceRow(
                cycle=int(row["cycle"]),
                stages=[None if row[s] == "-" else int(row[s]) for s in ("if", "id", "ex", "mem", "wb")],
                load_use=int(row["load_use"]),
                data_stall=int(row["data_stall"]),
                flush=int(row["flush"]),
                mem_stall=int(row["mem_stall"]),
                fetch_stall=int(row["fetch_stall"]),
                retired=int(row["retired"]),
            )
            for row in reader
        ]


def summarize(rows: Iterable[TraceRow], window: int = 1000, worst: int = 3) -> TraceSummary:
    rows = list(rows)
    summary = TraceSummary(pipeline_rows=len(rows))
    for row in rows:
        summary.retired += row.retired
        summary.load_use_stalls += row.load_use
        summary.data_hazard_stalls += row.data_stall
        summary.flush_cycles += row.flush
        summary.mem_stall_cycles += row.mem_stall
        summary.fetch_stall_cycles += row.fetch_stall
    # Memory freezes add cycles beyond the row itself, so the last row does not tell the
    # total unless its own freeze is added.
    if rows:
        summary.cycles = rows[-1].cycle + max(rows[-1].mem_stall, rows[-1].fetch_stall)
        # A pipeline needs 4 cycles to fill before its first retirement.
        summary.startup_cycles = 4
    explained = (summary.retired + summary.startup_cycles + summary.load_use_stalls + summary.data_hazard_stalls
                 + summary.flush_cycles + summary.mem_stall_cycles + summary.fetch_stall_cycles)
    summary.unexplained_cycles = summary.cycles - explained

    windows = []
    for start in range(0, len(rows), window):
        chunk = rows[start:start + window]
        if len(chunk) < window:
            break
        span = chunk[-1].cycle + max(chunk[-1].mem_stall, chunk[-1].fetch_stall) - chunk[0].cycle + 1
        windows.append({"first_cycle": chunk[0].cycle, "cycles": span, "ipc": sum(r.retired for r in chunk) / span})
    summary.worst_windows = sorted(windows, key=lambda w: w["ipc"])[:worst]
    return summary


def bottleneck_lines(summary: TraceSummary) -> List[str]:
    causes = [
        ("useful work", summary.retired),
        ("pipeline fill", summary.startup_cycles),
        ("load-use stalls", summary.load_use_stalls),
        ("data hazard stalls", summary.data_hazard_stalls),
        ("branch flushes", summary.flush_cycles),
        ("data memory stalls", summary.mem_stall_cycles),
        ("instruction fetch stalls", summary.fetch_stall_cycles),
    ]
    total = summary.cycles or 1
    return [f"  {name:<26}{count:>12,}  {100 * count / total:5.1f}%"
            for name, count in sorted(causes, key=lambda c: -c[1]) if count]


def diagram(rows: List[TraceRow], count: int) -> str:
    """Chart of the first `count` rows, with one line per instruction instance."""
    names = ["IF", "ID", "EX", "ME", "WB"]
    lanes: List[dict] = []  # {pc, cells}
    active: List[Optional[dict]] = [None] * 5
    shown = rows[:count]
    for column, row in enumerate(shown):
        next_active: List[Optional[dict]] = [None] * 5
        for stage in range(4, -1, -1):
            pc = row.stages[stage]
            if pc is None:
                continue
            lane = None
            # An instruction here last cycle stalled; one in the previous stage advanced.
            if stage > 0 and active[stage - 1] is not None and active[stage - 1]["pc"] == pc:
                lane = active[stage - 1]
            elif active[stage] is not None and active[stage]["pc"] == pc:
                lane = active[stage]
            if lane is None:
                lane = {"pc": pc, "cells": {}}
                lanes.append(lane)
            lane["cells"][column] = names[stage]
            next_active[stage] = lane
        active = next_active
    header = "pc    " + "".join(f"{row.cycle:>4}" for row in shown)
    lines = [header]
    for lane in lanes:
        lines.append(f"{lane['pc']:<6}" + "".join(f"{lane['cells'].get(c, '.'):>4}" for c in range(len(shown))))
    return "\n".join(lines)


def main(argv: Optional[list] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("trace", type=Path)
    parser.add_argument("--window", type=int, default=1000, help="rows per IPC window")
    parser.add_argument("--diagram", type=int, default=0, metavar="N", help="chart the first N cycles")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    rows = read_trace(args.trace)
    summary = summarize(rows, window=args.window)
    if args.json:
        print(json.dumps({**asdict(summary), "cpi": summary.cpi}))
    else:
        print(f"cycles {summary.cycles:,}  retired {summary.retired:,}  CPI {summary.cpi:.3f}")
        print("\n".join(bottleneck_lines(summary)))
        if summary.unexplained_cycles:
            print(f"  note: {summary.unexplained_cycles} cycles do not fit the model; the usual cause is a "
                  "program ending on a mispredicted branch, whose flush never delays anything")
        for w in summary.worst_windows:
            print(f"  low IPC window at cycle {w['first_cycle']:,}: IPC {w['ipc']:.3f}")
    if args.diagram:
        print(diagram(rows, args.diagram))
    return 0


if __name__ == "__main__":
    sys.exit(main())
