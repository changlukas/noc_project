"""Merge every continuous-injection result.csv into one table and plot it.

Globs rather than concatenating: `cat output/*/result.csv` would repeat the
header on every file.

Two curves per VC count, sharing the offered injection rate on x: accepted
throughput, whose knee is saturation, and mean latency, whose divergence is how
the literature defines the same point.
"""
import csv
import pathlib
import sys
from collections import defaultdict

_OUTPUT = pathlib.Path(__file__).resolve().parent.parent / "verilator" / "output"
_MERGED = pathlib.Path(__file__).resolve().parent / "injection_sweep.csv"


def load(pattern):
    rows = []
    for csv_path in sorted(_OUTPUT.glob("continuous_*/result.csv")):
        for row in csv.DictReader(csv_path.open()):
            if row["pattern"] == pattern:
                rows.append(row)
    if not rows:
        sys.exit(f"no continuous_*/result.csv rows for pattern {pattern!r}; run make sim-injection-sweep")
    return rows


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    pattern = args[0] if args else "uniform_random"
    rows = load(pattern)

    with _MERGED.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print(f"wrote {_MERGED} ({len(rows)} points, pattern={pattern})")

    by_vc = defaultdict(list)
    for row in rows:
        by_vc[int(row["vc"])].append(
            (float(row["injection_rate"]),
             float(row["accepted_bits_per_cycle"]),
             float(row["mean_latency"])))
    for vc in by_vc:
        by_vc[vc].sort()

    knobs = {(r["max_unique_ids"], r["max_outstanding"]) for r in rows}
    if len(knobs) != 1:
        print(f"WARNING: rows mix NSU settings {knobs}; the curves are not comparable")
    print(f"max_unique_ids={rows[0]['max_unique_ids']} max_outstanding={rows[0]['max_outstanding']}")

    print(f"\n{'rate':>6} " + " ".join(f"{'vc'+str(v):>18}" for v in sorted(by_vc)))
    rates = sorted({p[0] for pts in by_vc.values() for p in pts})
    for rate in rates:
        cells = []
        for vc in sorted(by_vc):
            hit = [p for p in by_vc[vc] if p[0] == rate]
            cells.append(f"{hit[0][1]:8.0f}/{hit[0][2]:<9.0f}" if hit else f"{'-':>18}")
        print(f"{rate:>6} " + " ".join(cells))
    print("\n(cells are accepted_bits_per_cycle / mean_latency)")

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("(matplotlib absent; the table above is the deliverable)")
        return

    dark = "--dark" in sys.argv
    surface, ink = ("#1a1a19", "#ffffff") if dark else ("#fcfcfb", "#0b0b0b")
    # Ordinal ramp, one blue hue, light->dark. vc1..vc8 in fixed order, never cycled.
    # Validated with the dataviz skill's validate_palette.js --ordinal against both
    # surfaces: monotone lightness, adjacent dL >= 0.06, single hue, and the step
    # nearest the surface clears 2:1. Dark is a separate selection, not a flip:
    # on a dark surface the brighter step is the prominent one.
    ramp = (["#184f95", "#2a78d6", "#6da7ec", "#b7d3f6"] if dark
            else ["#86b6ef", "#5598e7", "#2a78d6", "#104281"])

    vcs = sorted(by_vc)
    if len(vcs) > len(ramp):
        sys.exit(f"{len(vcs)} VC counts {vcs} exceed the {len(ramp)}-colour validated ramp; "
                  f"add a validated colour before plotting this many VC counts")

    # rcParams follow the plot-from-data line_training_curve reference
    # (github.com/Trae1ounG/paper-plot-skills). Parameter values only; that repo
    # ships no licence, so no code is copied from it.
    plt.rcParams.update({
        "font.family": "sans-serif",
        "font.sans-serif": ["DejaVu Sans", "Arial", "Helvetica"],
        "text.usetex": False,
        "axes.labelsize": 12,
        "axes.labelweight": "bold",
        "figure.facecolor": surface,
        "axes.facecolor": surface,
        "text.color": ink,
        "axes.labelcolor": ink,
        "xtick.color": ink,
        "ytick.color": ink,
    })

    # Two charts, never two y-scales: throughput and latency differ by orders of
    # magnitude, and a dual axis is the single most common charting error.
    fig, (ax_throughput, ax_latency) = plt.subplots(1, 2, figsize=(11, 4))
    # Per-axis list of (vc, color, x_end, y_end); labels are placed after all
    # lines are drawn, once each axis's y-range is known.
    line_ends = {ax_throughput: [], ax_latency: []}
    for color, vc in zip(ramp, vcs):
        xs = [p[0] for p in by_vc[vc]]
        ys_throughput = [p[1] for p in by_vc[vc]]
        ys_latency = [p[2] for p in by_vc[vc]]
        ax_throughput.plot(xs, ys_throughput, color=color, lw=2, marker="o", ms=4,
                            label=f"vc{vc}")
        ax_latency.plot(xs, ys_latency, color=color, lw=2, marker="o", ms=4,
                         label=f"vc{vc}")
        line_ends[ax_throughput].append((vc, color, xs[-1], ys_throughput[-1]))
        line_ends[ax_latency].append((vc, color, xs[-1], ys_latency[-1]))

    # Four series, so identity never rests on colour alone: a legend AND a
    # direct label at each line end, in the line's own colour. But a label that
    # overprints another is worse than no label, so skip one whose final y
    # lands within a small fraction of the axis's y-range of an already-placed
    # label. Whichever VC converges this sweep, this catches it without
    # hard-coding which series collide.
    label_collision_frac = 0.03
    axis_name = {ax_throughput: "throughput subplot", ax_latency: "latency subplot"}
    for ax, ends in line_ends.items():
        y_lo, y_hi = ax.get_ylim()
        threshold = label_collision_frac * (y_hi - y_lo)
        placed_ys = []
        skipped = []
        for vc, color, x_end, y_end in ends:
            if any(abs(y_end - placed) < threshold for placed in placed_ys):
                skipped.append(f"vc{vc}")
                continue
            placed_ys.append(y_end)
            ax.annotate(f"vc{vc}", xy=(x_end, y_end), xytext=(6, 0),
                        textcoords="offset points", color=color, va="center",
                        fontsize=9, fontweight="bold", clip_on=False)
        if skipped:
            print(f"skipped end label(s) on {axis_name[ax]} (converges with an "
                  f"already-labelled series): {', '.join(skipped)}")

    ax_throughput.set(xlabel="offered injection rate", ylabel="accepted throughput (bits/cycle)")
    ax_latency.set(xlabel="offered injection rate", ylabel="mean latency (cycles)")
    for ax in (ax_throughput, ax_latency):
        for sp in ax.spines.values():
            sp.set_visible(True)
            sp.set_linewidth(1.0)
            sp.set_color(ink)
        ax.tick_params(direction="out", length=4, width=0.8)
        ax.grid(False)
        # Right-hand margin so the end labels have room and don't get clipped.
        ax.margins(x=0.16)
        ax.legend(loc="best", facecolor=surface, edgecolor="0.6", labelcolor=ink)

    fig.suptitle(f"{pattern}, max_unique_ids={rows[0]['max_unique_ids']}, "
                 f"max_outstanding={rows[0]['max_outstanding']}", color=ink)
    fig.tight_layout()
    out = pathlib.Path(__file__).resolve().parent / (
        "injection_sweep_dark.png" if dark else "injection_sweep.png")
    fig.savefig(out, dpi=300, facecolor=surface)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
