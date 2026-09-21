#!/usr/bin/env python3
"""Export accepted direct measurements, route evidence and standalone figures.

Requires matplotlib for figures. Report prose remains manually authored.
"""
import argparse
import csv
import json
import os
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LABELS = {"gather": "Gather", "all_gather": "All-Gather", "reduce_scatter": "Reduce-Scatter",
          "reduce": "Reduce", "all_reduce": "All-Reduce"}


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")


def export(batch):
    data = ROOT / "docs/data"
    rows = json.loads((batch / "measurements.json").read_text())["records"]
    compact, diagnostics, routes = [], [], []
    status = json.loads((batch / "batch_status.json").read_text())
    for case in status:
        if case["status"] != "passed":
            raise ValueError("unaccepted case in batch")
        directory = batch / case["tag"]
        row = json.loads((directory / "validated.json").read_text())
        perf = json.loads((directory / "perf.json").read_text())
        expected = set(row["contract"]["costs"]["directed_link_bytes"])
        links = [link for link in perf["noc"]["links"]
                 if re.fullmatch(r"dat_\d+to\d+", link["name"]) and link["flit_count"] > 0]
        actual = {link["name"][4:] for link in links}
        if actual != expected:
            raise ValueError(f"DAT route mismatch: {case['tag']}")
        routes.append(dict(case=case["tag"], status="matched",
                           directed_links=sorted(actual), dat_flit_hops=sum(x["flit_count"] for x in links)))
    for row in rows:
        contract = row["contract"]
        cost = contract["costs"]
        directory = batch / row["directory"]
        source = os.path.relpath(directory, data)
        compact.append(dict(operation=contract["kind"], participants=len(contract["participants"]),
            elements_per_pe=contract["elements_per_pe"], bytes_per_pe=contract["per_pe_bytes"],
            root=contract["root"], execution_cycles=row["duration_cycles"],
            estimated_cycles=row["estimated_time_cycles"], slowdown=row["slowdown"],
            dat_flit_hops=row["dat_flit_hops"], C_bytes=cost["C_bytes"],
            E_byte_hops=cost["E_byte_hops"], N_directed_links=cost["N_directed_links"],
            L_hops=cost["L_hops"], traffic_depth=cost["traffic_depth"], source=source))
        perf = json.loads((directory / "perf.json").read_text())
        noc = perf["noc"]
        diagnostics.append(dict(operation=contract["kind"], participants=len(contract["participants"]),
            elements_per_pe=contract["elements_per_pe"], source=source, window=perf["window"],
            duration_cycles=row["duration_cycles"],
            active_links=[x for x in noc["links"] if re.fullmatch(r"dat_\d+to\d+", x["name"]) and x["flit_count"]],
            local_switches=[x for x in noc["router_dat_switches"] if x["port"] == "LOCAL"],
            allocation_waits=[x for x in noc["router_dat_allocations"] if x["waiting_cycles"]],
            credit_blocks=[x for x in noc["router_dat_output_vcs"] if x["credit_block_cycles"]],
            nmu_requests=noc["nmu_requests"]))
    with (data / "xy_direct_20260916.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(compact[0]))
        writer.writeheader()
        writer.writerows(compact)
    write_json(data / "xy_direct_diagnostics_20260916.json", diagnostics)
    write_json(data / "xy_direct_routes_20260916.json", routes)
    model = json.loads((batch / "model.json").read_text())
    for section in ("calibration", "heldout"):
        for sample in model[section]:
            sample["directory"] = os.path.relpath(sample["directory"], data)
    model["provenance"] = "Independent calibration and held-out records reused from xy_collectives_20260915"
    write_json(data / "xy_direct_model_20260916.json", model)
    return rows


def figures(rows, routes_only=False):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import FancyArrowPatch
    from fractions import Fraction
    folder = ROOT / "docs/figures/operations"
    fig, axes = plt.subplots(1, 3, figsize=(18, 6))
    panels = [("gather", "Gather / Reduce: root 5"),
              ("all_gather", "All-Gather / All-Reduce"), ("reduce_scatter", "Reduce-Scatter")]
    for ax, (kind, title) in zip(axes, panels):
        row = next(r for r in rows if r["contract"]["kind"] == kind and len(r["contract"]["participants"]) == 16)
        c = row["contract"]
        edges = c["costs"]["directed_link_bytes"]
        for name, volume in edges.items():
            a, b = map(int, name.split("to"))
            x, y, xx, yy = a % 4, a // 4, b % 4, b // 4
            dx, dy = xx-x, yy-y
            offset = 0.075 if f"{b}to{a}" in edges else 0
            x1, y1 = x-dy*offset+dx*.18, y+dx*offset+dy*.18
            x2, y2 = xx-dy*offset-dx*.18, yy+dx*offset-dy*.18
            ax.add_patch(FancyArrowPatch((x1,y1),(x2,y2),arrowstyle="-|>",
                                        mutation_scale=12,linewidth=1.35,color="#24658c"))
            value = str(Fraction(volume, c["per_pe_bytes"]))
            ax.text((x1+x2)/2-dy*.065,(y1+y2)/2+dx*.065,value,
                    ha="center",va="center",fontsize=8,color="#16415c",
                    bbox=dict(facecolor="white",edgecolor="none",pad=.4))
        for n in range(16):
            ax.scatter(n%4,n//4,s=380,color="#f3bd68" if n==c["root"] else "#e7eef3",edgecolor="#536575",zorder=4)
            ax.text(n%4,n//4,str(n),ha="center",va="center",fontsize=10,zorder=5)
        ax.set_title(title,fontsize=13,pad=12)
        ax.set_xlim(-.4,3.4); ax.set_ylim(-.4,3.4); ax.set_aspect("equal"); ax.axis("off")
    fig.suptitle(r"Direct XY baseline, P=16: complete-operation directed link payload / $S_{\mathrm{input}}$",fontsize=16)
    fig.text(.5,.035,r"$S_{\mathrm{input}}$ = Input size per tile (bytes). Arrows are physical router links; values are cumulative, not simultaneous rates.",ha="center",fontsize=11)
    fig.tight_layout(rect=(0,.07,1,.94))
    fig.savefig(folder / "xy_direct_routes.svg")
    fig.savefig(folder / "xy_direct_routes.png",dpi=130)
    plt.close(fig)
    if routes_only:
        return
    fig, axes = plt.subplots(1,2,figsize=(13,5),sharey=True)
    palette={"gather":"#9b4f00","all_gather":"#276a9c","reduce_scatter":"#27824d","reduce":"#aa4c78","all_reduce":"#6d5aa5"}
    for p,ax in zip((4,16),axes):
        for kind in LABELS:
            records=sorted([r for r in rows if r["contract"]["kind"]==kind and len(r["contract"]["participants"])==p],
                           key=lambda r:r["contract"]["elements_per_pe"])
            if not records: continue
            xs=[r["contract"]["elements_per_pe"] for r in records]
            ax.plot(xs,[r["duration_cycles"] for r in records],marker="o",label=LABELS[kind],color=palette[kind])
            ax.plot(xs,[r["estimated_time_cycles"] for r in records],linestyle="--",color=palette[kind],alpha=.7)
        ax.set_xscale("log"); ax.set_yscale("log"); ax.set_title(f"P={p}")
        ax.set_xlabel("Elements per PE (B)"); ax.grid(True,alpha=.2); ax.legend(fontsize=8)
    axes[0].set_ylabel("Completion time (cycles)")
    fig.suptitle(r"Direct XY: solid = Execution time, dashed = Estimated time")
    fig.text(.5,.015,"All-Gather and All-Reduce overlap; Gather and Reduce overlap at P=16 (identical direct traffic).",ha="center",fontsize=10)
    fig.tight_layout(rect=(0,.045,1,.95))
    fig.savefig(folder / "xy_direct_results.svg")
    fig.savefig(folder / "xy_direct_results.png",dpi=130)
    plt.close(fig)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("batch", type=Path)
    args = parser.parse_args()
    rows = export(args.batch.resolve())
    figures(rows)
    print(f"Exported {len(rows)} accepted primary results and route evidence")
