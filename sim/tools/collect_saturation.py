"""Collect saturation throughput per VC config from run-traffic logs.

Parses the axi_bw_monitor $display lines out of each VC config's run.log and
sums accepted bandwidth (read + write, aggregated across all manager nodes) into
a CSV. The measurement point is max concurrency (ids_per_tile=16 fills the
256-id space on a 16-node mesh) at greedy injection (inj=1.0), i.e. the fabric
saturation point. VC allocation is id-agnostic, so a higher VC count raising the
number here reflects reduced head-of-line blocking in the fabric.

Run the four points first (each builds + runs its VC config):
    for VC in vc1 vc2 vc4 vc8; do
      make -C sim/verilator run-traffic RUN_CLASS=directed \
          TOPOLOGY=mesh_4x4_$VC INJ_RATIO=1.0 IDS_PER_TILE=16 ...
    done
then: python3 collect_saturation.py
"""
import csv
import pathlib
import re
import sys

# axi_bw_monitor prints one line per node per direction:
#   [Monitor node<N>.manager][Read]  Latency: .., BW: <f> Bits/cycle, Util: ..%
#   [Monitor node<N>.manager][Write] Latency: .., BW: <f> Bits/cycle, Util: ..%
_BW = re.compile(r"\[Monitor[^\]]*\]\[(Read|Write)\].*?BW:\s*([\d.]+)\s*Bits/cycle", re.I)

VC_CONFIGS = ["mesh_4x4_vc1", "mesh_4x4_vc2", "mesh_4x4_vc4", "mesh_4x4_vc8"]
INJ_RATIO = "1.0"
IDS_PER_TILE = "16"
_OUTPUT_ROOT = pathlib.Path(__file__).resolve().parent.parent / "verilator" / "output"


def parse_bw(log_text: str) -> dict:
    """Sum read and write bandwidth (bits/cycle) across every node in the log."""
    read = write = 0.0
    for m in _BW.finditer(log_text):
        if m.group(1).lower() == "read":
            read += float(m.group(2))
        else:
            write += float(m.group(2))
    return {"read_bw": read, "write_bw": write}


def _log_path(topo: str) -> pathlib.Path:
    return _OUTPUT_ROOT / f"traffic_{topo}_r{INJ_RATIO}_id{IDS_PER_TILE}" / "run.log"


def main() -> None:
    rows = ["vc,inj_ratio,ids_per_tile,aggregate_bits_per_cyc,read_bits_per_cyc,write_bits_per_cyc"]
    for topo in VC_CONFIGS:
        p = _log_path(topo)
        if not p.exists():
            print(f"WARN: missing {p} -- run: make -C sim/verilator run-traffic "
                  f"RUN_CLASS=directed TOPOLOGY={topo} INJ_RATIO={INJ_RATIO} "
                  f"IDS_PER_TILE={IDS_PER_TILE}", file=sys.stderr)
            continue
        bw = parse_bw(p.read_text())
        agg = bw["read_bw"] + bw["write_bw"]
        vc = topo.split("_")[-1]
        rows.append(f"{vc},{INJ_RATIO},{IDS_PER_TILE},{agg:.1f},"
                    f"{bw['read_bw']:.1f},{bw['write_bw']:.1f}")
    out = pathlib.Path(__file__).resolve().parent / "saturation.csv"
    out.write_text("\n".join(rows) + "\n")
    print(f"wrote {out}")
    print("\n".join(rows))


if __name__ == "__main__":
    main()
