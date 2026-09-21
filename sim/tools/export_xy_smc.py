"""Publish numeric SMC evidence and its plot; report prose remains authored."""
import argparse
import csv
import json
import os
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("batch",type=Path)
    args=parser.parse_args()
    bundle=json.loads((args.batch/"measurements.json").read_text())
    data=ROOT/"docs/data"
    for section in ("records","accepted"):
        for row in bundle[section]:row["source"]=os.path.relpath(row["source"],data)
    (data/"xy_smc_20260916.json").write_text(json.dumps(bundle,indent=2)+"\n")
    fields=[k for k in bundle["records"][0] if k!="source_sha256"]
    with (data/"xy_smc_20260916.csv").open("w") as stream:
        writer=csv.DictWriter(stream,fieldnames=fields,extrasaction="ignore")
        writer.writeheader();writer.writerows(bundle["records"])
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    labels={"gather":"Gather", "all_gather":"All-Gather", "reduce_scatter":"Reduce-Scatter", "reduce":"Reduce", "all_reduce":"All-Reduce"}
    palette={"gather":"#9b4f00","all_gather":"#276a9c","reduce_scatter":"#27824d","reduce":"#aa4c78","all_reduce":"#6d5aa5"}
    fig,axes=plt.subplots(1,2,figsize=(13,5),sharey=True)
    for participants,ax in zip((4,16),axes):
        for op,label in labels.items():
            rows=sorted([r for r in bundle["records"] if r["operation"]==op and r["participants"]==participants],key=lambda r:r["elements_per_pe"])
            if not rows:continue
            xs=[r["elements_per_pe"] for r in rows]
            ax.plot(xs,[r["execution_cycles"] for r in rows],marker="o",color=palette[op],label=label)
            ax.plot(xs,[r["estimated_cycles"] for r in rows],linestyle="--",color=palette[op])
        ax.set_xscale("log");ax.set_yscale("log");ax.set_title(f"P={participants}")
        ax.set_xlabel("Elements per tile");ax.grid(alpha=.2);ax.legend(fontsize=8)
    axes[0].set_ylabel("Cycles")
    fig.suptitle("NI request acceptance to data delivery: measured (solid), ideal SMC (dashed)")
    fig.text(.5,.015,"Response return excluded from timing endpoint. Paired identical traffic curves overlap.",ha="center",fontsize=10)
    fig.tight_layout(rect=(0,.05,1,.95))
    for ext in ("svg","png"):fig.savefig(ROOT/f"docs/figures/operations/xy_smc_results.{ext}",dpi=140)
    print("Exported",len(bundle["records"]),"primary results")

if __name__=="__main__":main()
