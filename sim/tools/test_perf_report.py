import pathlib
import subprocess
import sys

import perf_report as pr

_COLS = ("topology,vc,router_vc_depth,ni_dat_rx_vc_depth,pattern,injection_mode,"
         "injection_rate,injection_count,seed,max_unique_ids,max_outstanding,"
         "max_txns_per_id,ids_per_initiator,burst_len,space,mst_stall_random,"
         "offered_flits_per_node_cycle,offered_bytes_per_node_cycle,"
         "accepted_bits_per_cycle,accepted_bytes_per_node_cycle,"
         "mean_latency_network,mean_latency_open")


def _point(root, pattern, offered, accepted_bytes, plat, seed=1, nlat=None):
    """One run directory holding the Task 1 result.csv column set."""
    d = root / f"continuous_mesh_4x4_{pattern}_r{offered}_s{seed}"
    d.mkdir(parents=True)
    row = (f"mesh_4x4,2,8,8,{pattern},1,{offered},200,{seed},1,32,32,1,32,memory,0,"
           f"{offered},{offered * 64 * 33 * 2},{accepted_bytes * 8 * 16},{accepted_bytes},"
           f"{plat if nlat is None else nlat},{plat}")
    (d / "result.csv").write_text(_COLS + "\n" + row + "\n")
    return d


def _curve(root, pattern="uniform_random"):
    for offered, plat, acc in zip((0.1, 0.2, 0.3, 0.4, 0.5),
                                  (40.0, 42.0, 50.0, 130.0, 400.0),
                                  (5.0, 10.0, 20.0, 21.0, 21.0)):
        _point(root, pattern, offered, acc, plat)


_KEY = ("mesh_4x4", "2", "8", "8", "32", "32", "1", "32", "200", "0")


def test_curve_marks_saturation(tmp_path):
    _curve(tmp_path)
    sat = pr.saturation(pr.collect(tmp_path)[_KEY]["uniform_random"])
    # Zero load plat is 40 at the lowest offered point, so the 3x threshold is
    # 120, crossed between offered 0.3 (plat 50) and 0.4 (plat 130).
    # f = (120 - 50) / (130 - 50) = 0.875.
    assert abs(sat["offered"] - 0.3875) < 1e-9
    assert abs(sat["accepted_bytes"] - 20.875) < 1e-9


def _row(text, heading, first_cell):
    """The cells of the one row of `heading`'s table whose first cell matches."""
    body = text.split(heading, 1)[1]
    for line in body.splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if cells and cells[0].rstrip("*") == first_cell:
            return cells
    raise AssertionError(f"no {first_cell!r} row under {heading!r}")


def test_curve_that_never_reaches_3x_reports_a_bound(tmp_path):
    """A curve still climbing at the last point has no saturation load, only a
    lower bound: the row says `> <max offered>` rather than inventing one."""
    for offered, plat in zip((0.1, 0.2, 0.3), (40.0, 42.0, 50.0)):
        _point(tmp_path, "tornado", offered, 10.0, plat)
    text = pr.report(tmp_path)
    assert pr.saturation(pr.collect(tmp_path)[_KEY]["tornado"]) is None
    assert "> 0.300" in text and "never reaches 3x zero load" in text


def test_zero_load_probes_report_nlat_and_no_plat(tmp_path):
    """The narrow and data probes predate the open-loop tb, so they carry no
    [SrcQueue] line and section 2 shows nlat with plat marked absent."""
    _curve(tmp_path)
    (tmp_path / "s2zl_va_narrow").mkdir()
    (tmp_path / "s2zl_va_narrow" / "run.log").write_text(
        "[Monitor node0.master][Read] Latency: 31.01 +- 0.14, N: 200, BW: 4.65 Bits/cycle\n"
        "[Monitor node0.master][Write] Latency: 32.00 +- 0.00, N: 200, BW: 4.65 Bits/cycle\n")
    cells = _row(pr.report(tmp_path), "## 2 Zero-load latency", "s2zl_va_narrow")
    assert cells[1] == "narrow"
    assert cells[2] == "31.0" and cells[3] == "-"
    assert cells[4] == "32.0" and cells[5] == "-"


def test_pattern_summary_percent_of_ideal(tmp_path):
    # neighbor has ideal 1.0 flits per node per cycle and no self traffic, so
    # the served share is the whole pattern. 0.6 accepted flits per node per
    # cycle is 0.6 * 64 * 66 / 67 B, the inverse of the 34/33 flit conversion.
    _point(tmp_path, "neighbor", 0.9, 0.6 * 64 * 66 / 67, 90.0)
    cells = _row(pr.report(tmp_path), "## 4 Pattern summary", "neighbor")
    assert cells[5] == "0.600"   # accepted flits at the highest offered load
    assert cells[6] == "60"      # 0.600 / (1.000 served * 1.000 ideal)


def test_seed_spread_column(tmp_path):
    _curve(tmp_path)
    _point(tmp_path, "uniform_random", 0.3, 20.0, 60.0, seed=2)
    # The rate 0.3 point now carries seeds 1 and 2 with plat 50 and 60: the
    # cell is their mean and the spread is max minus min.
    cells = _row(pr.report(tmp_path), "### uniform_random", "0.300")
    assert cells[5] == "55.0" and cells[6] == "10.0" and cells[7] == "2"


def test_no_removed_names():
    """The clean cut: no tracked file under Makefile, sim/, docs/ or README.md
    still names a removed script or the old report.

    Two exclusions. This file, because a check that forbids four strings has to
    name them. docs/superpowers/, because those are the archived plan and spec
    records of the campaigns that created and then removed those files, and the
    spec driving this deletion is itself one of them."""
    self_path = pathlib.Path(__file__).name
    root = pathlib.Path(__file__).resolve().parents[2]
    names = "summarize_results|plot_injection_sweep|perf_cli_summary|sweep_summary"
    grep = subprocess.run(
        ["git", "grep", "-nE", names, "--", "Makefile", "sim", "docs", "README.md",
         ":(exclude)docs/superpowers", f":(exclude)sim/tools/{self_path}"],
        cwd=root, capture_output=True, encoding="utf-8", errors="replace")
    # git grep exits 1 on no match, 0 on a match, 2 and up on a real failure.
    assert grep.returncode == 1, grep.stdout or grep.stderr


if __name__ == "__main__":
    sys.exit(subprocess.call([sys.executable, "-m", "pytest", "-q", __file__]))
