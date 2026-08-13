"""Plot sustained bandwidth vs per-ID outstanding window (Little's law sweep).

Parses [Monitor nodeN.master][Read|Write] lines from continuous_* run.log files
whose seed encodes the swept MAX_TXNS_PER_ID (seed = 300 + N), since
emit_result_csv.py does not record max_txns_per_id. Emits window_sweep.png
next to this script.
"""
import pathlib
import re
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

_OUTPUT = pathlib.Path(__file__).resolve().parent.parent / "verilator" / "output"
_PNG = pathlib.Path(__file__).resolve().parent / "window_sweep.png"
_SEED_BASE = 300
_TAG = re.compile(r"_s(\d+)$")
_MON = re.compile(
    r"\[Monitor node\d+\.master\]\[(Read|Write)\] Latency: ([\d.]+) \+- [\d.]+, "
    r"N: (\d+), BW: ([\d.]+) Bits/cycle")


def load():
    points = {}
    for log in sorted(_OUTPUT.glob("continuous_*/run.log")):
        m = _TAG.search(log.parent.name)
        if not m:
            continue
        seed = int(m.group(1))
        if seed <= _SEED_BASE or seed > _SEED_BASE + 64:
            continue
        window = seed - _SEED_BASE
        acc = {"Read": [0.0, 0.0, 0], "Write": [0.0, 0.0, 0]}   # bw_sum, lat_wsum, n_sum
        for cls, lat, n, bw in _MON.findall(log.read_text()):
            acc[cls][0] += float(bw)
            acc[cls][1] += float(lat) * int(n)
            acc[cls][2] += int(n)
        if acc["Read"][2] == 0:
            continue
        nodes = 16
        points[window] = {
            "read_bw": acc["Read"][0] / nodes / 8,
            "write_bw": acc["Write"][0] / nodes / 8,
            "read_lat": acc["Read"][1] / acc["Read"][2],
            "write_lat": acc["Write"][1] / acc["Write"][2],
        }
    if not points:
        sys.exit("no continuous_*_s3xx/run.log found; run the window sweep first")
    return dict(sorted(points.items()))


def main():
    pts = load()
    win = list(pts)
    fig, (ax_bw, ax_lat) = plt.subplots(1, 2, figsize=(11, 4.2))

    ax_bw.plot(win, [p["read_bw"] for p in pts.values()], "o-", label="read")
    ax_bw.plot(win, [p["write_bw"] for p in pts.values()], "s-", label="write")
    ax_bw.set_xlabel("max_txns_per_id (outstanding window)")
    ax_bw.set_ylabel("accepted payload bandwidth (B/cycle/node)")
    ax_bw.set_xscale("log", base=2)
    ax_bw.grid(True, alpha=0.3)
    ax_bw.legend()
    ax_bw.set_title("sustained bandwidth vs outstanding window")

    ax_lat.plot(win, [p["read_lat"] for p in pts.values()], "o-", label="read")
    ax_lat.plot(win, [p["write_lat"] for p in pts.values()], "s-", label="write")
    ax_lat.set_xlabel("max_txns_per_id (outstanding window)")
    ax_lat.set_ylabel("mean transaction latency (cycles)")
    ax_lat.set_xscale("log", base=2)
    ax_lat.grid(True, alpha=0.3)
    ax_lat.legend()
    ax_lat.set_title("latency vs outstanding window")

    fig.tight_layout()
    fig.savefig(_PNG, dpi=150)
    print(f"wrote {_PNG}")
    for w, p in pts.items():
        print(f"  window={w:>2}  read={p['read_bw']:6.2f} B/c  write={p['write_bw']:6.2f} B/c  "
              f"lat R/W={p['read_lat']:7.1f}/{p['write_lat']:7.1f}")


if __name__ == "__main__":
    main()
