#!/usr/bin/env python3
import subprocess, sys
from pathlib import Path
root = Path(__file__).resolve().parents[1]
sim = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "build" / "sim"
program = root / "programs" / "loop.asm"
configs = [
    ("pipeline, always not-taken, 4x1 cache", ["--pipeline", "--predictor", "not-taken", "--sets", "4", "--ways", "1"]),
    ("pipeline, 2-bit predictor, 4x1 cache", ["--pipeline", "--predictor", "twobit", "--sets", "4", "--ways", "1"]),
    ("single-cycle, 16-set 2-way cache", ["--no-pipeline", "--sets", "16", "--ways", "2", "--block", "1"]),
]
if not sim.exists():
    sys.exit(f"simulator binary not found: {sim}")
print(f"{'configuration':<42} metrics")
for name, args in configs:
    out = subprocess.check_output([str(sim), str(program), *args], text=True).strip()
    print(f"{name:<42} {out}")
