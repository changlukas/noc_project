"""Bar chart of aggregate saturation throughput per VC config, from saturation.csv.

Throwaway: prints a table always; renders saturation.png if matplotlib is present.
"""
import csv
import pathlib

_CSV = pathlib.Path(__file__).resolve().parent / "saturation.csv"


def saturation_per_vc(csv_path=_CSV) -> dict:
    out = {}
    for row in csv.DictReader(open(csv_path)):
        out[row["vc"]] = float(row["aggregate_bits_per_cyc"])
    return out


def main() -> None:
    sat = saturation_per_vc()
    base = next(iter(sat.values())) if sat else 0.0
    for vc, v in sat.items():
        delta = f" (+{(v / base - 1) * 100:.0f}% vs {next(iter(sat))})" if base else ""
        print(f"{vc}: saturation throughput = {v:.0f} bits/cyc{delta}")
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        plt.bar(list(sat.keys()), list(sat.values()))
        plt.ylabel("aggregate saturation throughput (bits/cyc)")
        plt.title("Saturation throughput per VC (4x4 uniform_random, ids/tile=16, inj=1.0)")
        out = pathlib.Path(__file__).resolve().parent / "saturation.png"
        plt.savefig(out)
        print(f"wrote {out}")
    except ImportError:
        print("(matplotlib absent; table above is the deliverable)")


if __name__ == "__main__":
    main()
