import sys

import summarize_results as s


def _write_csv(run_dir, pattern, seed, space="memory", vc="2", burst="0",
               bw="100.0", lat="50.0"):
    run_dir.mkdir(parents=True)
    header = ("topology,vc,router_vc_depth,pattern,injection_mode,"
              "injection_rate,injection_count,seed,max_unique_ids,"
              "max_outstanding,max_txns_per_id,ids_per_initiator,burst_len,"
              "space,accepted_bits_per_cycle,mean_latency")
    row = (f"mesh_2x2,{vc},8,{pattern},1,0.9,200,{seed},1,32,32,1,{burst},"
           f"{space},{bw},{lat}")
    (run_dir / "result.csv").write_text(header + "\n" + row + "\n")


def test_default_and_diff_labels(tmp_path, capsys, monkeypatch):
    _write_csv(tmp_path / "continuous_mesh_2x2_neighbor_r0.9_s1", "neighbor", 1)
    _write_csv(tmp_path / "continuous_mesh_2x2_neighbor_r0.9_s2", "neighbor", 2,
               vc="8", burst="32")
    monkeypatch.setattr(sys, "argv", ["summarize_results", str(tmp_path)])
    s.main()
    out = capsys.readouterr().out
    # The all-defaults group is named "default" and sorts first; the other
    # group is named by its diffs against the shipped defaults.
    assert "## Set 1: default" in out
    assert "## Set 2: vc=8, burst_len=32" in out
    assert "| neighbor" in out


def test_narrow_runs_add_the_latency_compare_columns(tmp_path, capsys,
                                                     monkeypatch):
    _write_csv(tmp_path / "continuous_mesh_2x2_neighbor_r0.9_s1", "neighbor", 1,
               lat="50.0")
    _write_csv(tmp_path / "continuous_mesh_2x2_neighbor_narrow_r0.9_s2",
               "neighbor", 2, space="config", lat="40.0")
    monkeypatch.setattr(sys, "argv", ["summarize_results", str(tmp_path)])
    s.main()
    out = capsys.readouterr().out
    # Same parameter set, both classes: one row with data and narrow columns
    # and their difference. Without any narrow run those columns are omitted
    # (test_default_and_diff_labels' output has no such header).
    assert "narrow lat (cyc)" in out
    assert "-10.0" in out
