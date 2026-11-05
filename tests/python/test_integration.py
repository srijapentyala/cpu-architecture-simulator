"""Drives the built simulator. Expected answers are computed here in plain Python, so an
assembly program or simulator bug cannot hide by agreeing with itself."""
import csv
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import parse_trace  # noqa: E402
import run_benchmarks  # noqa: E402

SIM = Path(os.environ.get("SIM_BINARY", ROOT / "build" / "sim"))
PROGRAMS = ROOT / "programs"

pytestmark = pytest.mark.skipif(not SIM.exists(), reason=f"simulator not built: {SIM}")


def run_sim(program, *args):
    out = subprocess.run([str(SIM), str(PROGRAMS / f"{program}.asm"), "--mem-words", "65536", "--json", *args],
                         check=True, capture_output=True, text=True).stdout
    return json.loads(out)


def lcg_values(n):
    values, x = [], 1
    for _ in range(n):
        x = (x * 75 + 74) & 65535
        values.append(x)
    return values


def expected_matmul_checksum(n=24):
    a = [[i + j + 1 for j in range(n)] for i in range(n)]
    b = [[i * j for j in range(n)] for i in range(n)]
    return sum(sum(a[i][k] * b[k][j] for k in range(n)) for i in range(n) for j in range(n))


def expected_pointer_chase():
    p, total = 0, 0
    for _ in range(50000):
        p = ((p // 4 + 1597) & 4095) * 4
        total += p
    return p, total


ANSWERS = {
    "seq_sum": lambda r: r[10] == 4 * sum(range(2048)),
    "stride_sum": lambda r: r[10] == 16 * sum(range(0, 2048, 16)),
    "matmul": lambda r: r[30] == expected_matmul_checksum(),
    "bubble_sort": lambda r: r[10] == 1 and r[11] == sum(lcg_values(256)),
    "pointer_chase": lambda r: (r[5], r[9]) == expected_pointer_chase(),
    "branch_patterns": lambda r: r[10] == 15000 and r[11] == 10000,
    "loop": lambda r: r[1] == 8,
}

CONFIG_NAMES = sorted(run_benchmarks.CONFIGS)


@pytest.mark.parametrize("program", sorted(ANSWERS))
@pytest.mark.parametrize("config", CONFIG_NAMES)
def test_every_program_gets_the_right_answer_under_every_config(program, config):
    metrics = run_sim(program, *run_benchmarks.CONFIGS[config])
    assert ANSWERS[program](metrics["regs"])


def test_every_program_in_the_directory_has_an_expected_answer():
    assert {p.stem for p in PROGRAMS.glob("*.asm")} == set(ANSWERS)


@pytest.mark.parametrize("program", sorted(set(ANSWERS) - {"loop"}))
def test_cycles_are_fully_explained_by_the_stall_counters(program):
    m = run_sim(program, *run_benchmarks.CONFIGS["l1i-l2"])
    assert m["cycles"] == (m["retired"] + 4 + m["load_use_stalls"] + m["data_hazard_stalls"] + m["flush_cycles"]
                           + m["mem_stall_cycles"] + m["fetch_stall_cycles"])


def test_gshare_beats_bimodal_on_periodic_branches():
    bimodal = run_sim("branch_patterns", "--predictor", "twobit")
    gshare = run_sim("branch_patterns", "--predictor", "gshare")
    assert gshare["mispredict_rate"] < bimodal["mispredict_rate"] / 2


def test_larger_blocks_help_sequential_access_more_than_strided_access():
    def miss(program, block, sets):
        return run_sim(program, "--block", str(block), "--sets", str(sets))["levels"][0]["miss_rate"]

    # Same capacity both ways: 128 words.
    sequential_gain = miss("seq_sum", 16, 8) / miss("seq_sum", 1, 128)
    strided_gain = miss("stride_sum", 16, 8) / miss("stride_sum", 1, 128)
    assert sequential_gain < 0.1
    assert sequential_gain < strided_gain


def test_a_stride_that_maps_to_few_sets_is_a_conflict_problem():
    # Reading every 16th word touches only 8 of 128 one-word sets, so each set thrashes.
    # With 8 sets of 16-word blocks, the same reads spread across every set.
    args = ["--mem-latency", "1"]
    conflicted = run_sim("stride_sum", "--block", "1", "--sets", "128", *args)
    spread = run_sim("stride_sum", "--block", "16", "--sets", "8", *args)
    assert conflicted["levels"][0]["miss_rate"] == 1.0
    assert spread["levels"][0]["miss_rate"] < conflicted["levels"][0]["miss_rate"]


def test_forwarding_lowers_cpi_without_changing_the_answer():
    with_fwd = run_sim("matmul")
    without = run_sim("matmul", "--no-forwarding")
    assert with_fwd["cpi"] < without["cpi"]
    assert with_fwd["regs"] == without["regs"]


def test_an_l2_reduces_memory_stalls():
    args = ["--sets", "16", "--ways", "1", "--block", "2", "--mem-latency", "50"]
    assert (run_sim("matmul", *args, "--l2", "512,4,8,6")["mem_stall_cycles"]
            < run_sim("matmul", *args)["mem_stall_cycles"])


def test_write_back_writes_memory_less_than_write_through():
    assert run_sim("seq_sum", "--write", "wb", "--sets", "64", "--block", "4")["mem_writes"] < \
        run_sim("seq_sum", "--write", "wt", "--sets", "64", "--block", "4")["mem_writes"]


def test_bad_config_exits_nonzero_with_a_message():
    result = subprocess.run([str(SIM), str(PROGRAMS / "loop.asm"), "--predictor", "psychic"],
                            capture_output=True, text=True)
    assert result.returncode != 0
    assert "bad predictor" in result.stderr


def test_out_of_range_access_is_reported_not_ignored():
    result = subprocess.run([str(SIM), str(PROGRAMS / "seq_sum.asm"), "--mem-words", "16"],
                            capture_output=True, text=True)
    assert result.returncode != 0
    assert "out of range" in result.stderr


@pytest.fixture
def trace(tmp_path):
    def make(program, *args):
        path = tmp_path / "trace.csv"
        metrics = run_sim(program, "--trace", str(path), *args)
        return metrics, parse_trace.read_trace(path)
    return make


@pytest.mark.parametrize("config", ["base", "no-forwarding", "l1i-l2", "pred-not-taken"])
def test_trace_totals_agree_with_the_simulators_own_metrics(trace, config):
    metrics, rows = trace("bubble_sort", *run_benchmarks.CONFIGS[config])
    summary = parse_trace.summarize(rows)
    assert summary.cycles == metrics["cycles"]
    assert summary.retired == metrics["retired"]
    assert summary.load_use_stalls == metrics["load_use_stalls"]
    assert summary.data_hazard_stalls == metrics["data_hazard_stalls"]
    assert summary.flush_cycles == metrics["flush_cycles"]
    assert summary.mem_stall_cycles == metrics["mem_stall_cycles"]
    assert summary.fetch_stall_cycles == metrics["fetch_stall_cycles"]
    assert summary.unexplained_cycles == 0


def test_trace_shows_an_instruction_moving_through_the_stages_in_order(trace):
    _, rows = trace("seq_sum")
    # pc 0 fetches in cycle 1 and must be in stage k at cycle k + 1.
    for stage, row in enumerate(rows[:5]):
        assert row.stages[stage] == 0


def test_diagram_draws_a_load_use_bubble(tmp_path):
    source = tmp_path / "p.asm"
    source.write_text("LW r1, 0(r0)\nADD r2, r1, r1\nNOP\n")
    path = tmp_path / "t.csv"
    subprocess.run([str(SIM), str(source), "--trace", str(path)], check=True, capture_output=True)
    chart = parse_trace.diagram(parse_trace.read_trace(path), 8).splitlines()
    assert chart[1].split() == ["0", "IF", "ID", "EX", "ME", "WB", ".", ".", "."]
    # ADD waits in ID for one extra cycle, then reads the load through forwarding.
    assert chart[2].split() == ["1", ".", "IF", "ID", "ID", "EX", "ME", "WB", "."]


def test_read_trace_rejects_a_file_that_is_not_a_trace(tmp_path):
    path = tmp_path / "x.csv"
    path.write_text("a,b\n1,2\n")
    with pytest.raises(ValueError):
        parse_trace.read_trace(path)


def test_run_matrix_refuses_a_config_that_changes_the_answer(tmp_path):
    fake = tmp_path / "sim.py"
    fake.write_text(
        "#!/usr/bin/env python3\nimport json, sys\n"
        "reg = 1 if '--bad' in sys.argv else 0\n"
        "print(json.dumps({'cycles': 1, 'retired': 1, 'cpi': 1, 'mispredict_rate': 0, 'load_use_stalls': 0,"
        "'data_hazard_stalls': 0, 'flush_cycles': 0, 'mem_stall_cycles': 0, 'fetch_stall_cycles': 0,"
        "'mem_reads': 0, 'mem_writes': 0, 'levels': [], 'regs': [reg]}))\n"
    )
    fake.chmod(0o755)
    with pytest.raises(RuntimeError, match="changed the program's result"):
        run_benchmarks.run_matrix(fake, [PROGRAMS / "loop.asm"], {"a": [], "b": ["--bad"]})


def test_benchmark_csv_is_reproducible(tmp_path):
    programs = tmp_path / "programs"
    programs.mkdir()
    (programs / "loop.asm").write_text((PROGRAMS / "loop.asm").read_text())
    for out in ("one", "two"):
        assert run_benchmarks.main(["--sim", str(SIM), "--programs", str(programs), "--out", str(tmp_path / out)]) == 0
    first = (tmp_path / "one" / "benchmarks.csv").read_text()
    assert first == (tmp_path / "two" / "benchmarks.csv").read_text()
    rows = list(csv.DictReader(first.splitlines()))
    assert {r["config"] for r in rows} == set(run_benchmarks.CONFIGS)
