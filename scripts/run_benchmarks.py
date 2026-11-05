#!/usr/bin/env python3
"""Run every benchmark program under every configuration and compare the results.

    python3 scripts/run_benchmarks.py                 # writes results/benchmarks.csv
    python3 scripts/run_benchmarks.py --sim build/sim --out results --markdown

Results depend only on the programs and the configuration, so the same inputs reproduce
the same CSV byte for byte. Each row also records a hash of the final registers, and a
configuration that changed a program's answer is an error, not a data point.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional

ROOT = Path(__file__).resolve().parents[1]
MEM_WORDS = "65536"

# A small but plausible memory system: 20 cycles to memory, with L1s that have real
# miss rates on these programs. Each configuration changes one thing against the base.
BASE = ["--predictor", "twobit", "--sets", "32", "--ways", "2", "--block", "4", "--write", "wb", "--mem-latency", "20"]


def with_args(**overrides: str) -> List[str]:
    args = list(BASE)
    for key, value in overrides.items():
        flag = "--" + key.replace("_", "-")
        if flag in args:
            args[args.index(flag) + 1] = value
        else:
            args += [flag, value]
    return args


CONFIGS: Dict[str, List[str]] = {
    "base": BASE,
    # Branch prediction
    "pred-not-taken": with_args(predictor="not-taken"),
    "pred-btfn": with_args(predictor="btfn"),
    "pred-gshare": with_args(predictor="gshare"),
    "pred-tournament": with_args(predictor="tournament"),
    # Hazard handling
    "no-forwarding": BASE + ["--no-forwarding"],
    # Cache geometry
    "l1-direct": with_args(sets="64", ways="1"),
    "l1-4way": with_args(sets="16", ways="4"),
    "l1-block1": with_args(sets="128", block="1"),
    "l1-block16": with_args(sets="8", block="16"),
    "l1-large": with_args(sets="256"),
    # Write policy
    "l1-write-through": with_args(write="wt"),
    # Hierarchy
    "l2": BASE + ["--l2", "256,4,8,6"],
    "l1i-l2": BASE + ["--l1i", "32,2,4", "--l2", "256,4,8,6"],
    # Reference
    "single-cycle": BASE + ["--no-pipeline"],
}


def run_one(sim: Path, program: Path, args: List[str]) -> dict:
    out = subprocess.run([str(sim), str(program), "--mem-words", MEM_WORDS, "--json", *args],
                         check=True, capture_output=True, text=True).stdout
    return json.loads(out)


def flatten(program: str, config: str, metrics: dict) -> dict:
    row = {
        "program": program,
        "config": config,
        "cycles": metrics["cycles"],
        "retired": metrics["retired"],
        "cpi": round(metrics["cpi"], 4),
        "mispredict_rate": round(metrics["mispredict_rate"], 4),
        "load_use_stalls": metrics["load_use_stalls"],
        "data_hazard_stalls": metrics["data_hazard_stalls"],
        "flush_cycles": metrics["flush_cycles"],
        "mem_stall_cycles": metrics["mem_stall_cycles"],
        "fetch_stall_cycles": metrics["fetch_stall_cycles"],
        "mem_reads": metrics["mem_reads"],
        "mem_writes": metrics["mem_writes"],
    }
    levels = {level["name"]: level for level in metrics["levels"]}
    for name in ("L1I", "L1D", "L2"):
        row[f"{name}_miss_rate"] = round(levels[name]["miss_rate"], 4) if name in levels else ""
    row["regs_sha1"] = hashlib.sha1(json.dumps(metrics["regs"]).encode()).hexdigest()[:12]
    return row


def run_matrix(sim: Path, programs: List[Path], configs: Dict[str, List[str]]) -> List[dict]:
    rows = []
    for program in programs:
        expected = None
        for name, args in configs.items():
            row = flatten(program.stem, name, run_one(sim, program, args))
            if expected is None:
                expected = row["regs_sha1"]
            elif row["regs_sha1"] != expected:
                raise RuntimeError(f"{program.stem}: config {name} changed the program's result")
            rows.append(row)
    return rows


def write_csv(rows: List[dict], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def cpi_table(rows: List[dict]) -> str:
    programs = sorted({r["program"] for r in rows})
    configs = list(dict.fromkeys(r["config"] for r in rows))
    lookup = {(r["program"], r["config"]): r for r in rows}
    lines = ["| config | " + " | ".join(programs) + " |", "|---|" + "---|" * len(programs)]
    for config in configs:
        cells = [f"{lookup[(p, config)]['cpi']:.3f}" for p in programs]
        lines.append(f"| {config} | " + " | ".join(cells) + " |")
    return "\n".join(lines)


def bottlenecks(rows: List[dict], config: str = "base") -> str:
    lines = ["| program | CPI | useful | load-use | flush | memory | biggest loss |", "|---|---|---|---|---|---|---|"]
    for r in sorted((r for r in rows if r["config"] == config), key=lambda r: r["program"]):
        total = r["cycles"]
        shares = {
            "load-use": r["load_use_stalls"] / total,
            "branch flush": r["flush_cycles"] / total,
            "memory": (r["mem_stall_cycles"] + r["fetch_stall_cycles"]) / total,
        }
        worst = max(shares, key=lambda k: shares[k])
        lines.append(
            f"| {r['program']} | {r['cpi']:.3f} | {r['retired'] / total:.0%} | {shares['load-use']:.0%} | "
            f"{shares['branch flush']:.0%} | {shares['memory']:.0%} | {worst} |"
        )
    return "\n".join(lines)


def main(argv: Optional[list] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--sim", type=Path, default=ROOT / "build" / "sim")
    parser.add_argument("--programs", type=Path, default=ROOT / "programs")
    parser.add_argument("--out", type=Path, default=ROOT / "results")
    parser.add_argument("--markdown", action="store_true", help="also write results/summary.md")
    args = parser.parse_args(argv)

    if not args.sim.exists():
        print(f"simulator binary not found: {args.sim}", file=sys.stderr)
        return 1
    programs = sorted(args.programs.glob("*.asm"))
    if not programs:
        print(f"no programs in {args.programs}", file=sys.stderr)
        return 1
    rows = run_matrix(args.sim, programs, CONFIGS)
    write_csv(rows, args.out / "benchmarks.csv")
    summary = "## CPI by configuration\n\n" + cpi_table(rows) + "\n\n## Where the base configuration loses cycles\n\n" + bottlenecks(rows) + "\n"
    print(summary)
    if args.markdown:
        (args.out / "summary.md").write_text(summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
