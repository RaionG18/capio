#!/usr/bin/env python3
"""Plots of the Leonardo campaign.

    plot_scaling.py OUT_DIR RUN_DIR [RUN_DIR ...]

Reads the run directories (results.csv + experiments.csv, as written by the
harness) and writes, for each figure, a PNG, a PDF and the CSV of the plotted
numbers (mean and stddev over repetitions):

  weak_seq_throughput   weak scaling (4 pairs per node), producers then consumers:
                        aggregate consumer bandwidth vs nodes
  weak_seq_efficiency   the same as efficiency: per-node bandwidth / that at 1 node
  weak_conc_throughput  weak scaling, CAPIO streaming: end-to-end bandwidth vs nodes
  strong_seq_time       strong scaling (32 pairs in total): time to write and read
                        the whole data set vs nodes, with the ideal T1/N
  strong_seq_speedup    the same as speedup T1/TN, with the ideal N
  intranode_scaling     one node, 1-32 pairs: aggregate consumer bandwidth
  single_node_chunk     one node, one pair: consumer bandwidth vs chunk size

Only experiments with status ok count. Metrics as in summarize.py: aggregate
bandwidth = bytes / (last end - first start) over all processes of a role;
streaming end-to-end = bytes / (last consumer end - first producer start).
"""
import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent))
from summarize import per_experiment  # noqa: E402

# One color and one marker per configuration, in every figure (validated
# categorical palette, slots 1-4; markers keep the series apart in grayscale).
SERIES = {
    "fs-lustre": dict(label="Lustre (POSIX)", color="#2a78d6", marker="o"),
    "fs-tmp": dict(label="Node-local disk (POSIX)", color="#eb6834", marker="s"),
    "capio-posix": dict(label="CAPIO, POSIX", color="#1baf7a", marker="^"),
    "capio-io_uring": dict(label="CAPIO, io_uring", color="#eda100", marker="D"),
}
TEXT, MUTED, GRID, SURFACE = "#0b0b0b", "#52514e", "#e4e3df", "#fcfcfb"

plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "axes.edgecolor": MUTED, "axes.labelcolor": TEXT, "text.color": TEXT,
    "xtick.color": MUTED, "ytick.color": MUTED, "font.size": 10,
    "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8,
    "legend.frameon": False, "lines.linewidth": 2, "lines.markersize": 7,
})


def series_of(info):
    return info["config"] if info["config"] != "capio" else f"capio-{info['engine']}"


def collect(runs):
    exps = []
    for run in runs:
        exps += per_experiment(Path(run)).values()
    return exps


def stats(values):
    if not values:
        return None
    sd = statistics.stdev(values) if len(values) > 1 else 0.0
    return statistics.mean(values), sd, len(values)


def total_time(info):
    """Seconds from the first producer start to the last consumer end."""
    r = info["roles"]
    return r["consumer"]["end"] - r["producer"]["start"]


def group(exps, select, xkey, metric):
    """{series: {x: (mean, sd, n)}} over the experiments that pass select."""
    acc = defaultdict(lambda: defaultdict(list))
    for e in exps:
        if not select(e):
            continue
        value = metric(e)
        if value is not None:
            acc[series_of(e)][int(e[xkey])].append(value)
    return {s: {x: stats(v) for x, v in sorted(xs.items())} for s, xs in acc.items()}


def write_csv(path, data, xname, yname):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["series", xname, f"{yname}_mean", f"{yname}_sd", "reps"])
        for s in SERIES:
            for x, (m, sd, n) in data.get(s, {}).items():
                w.writerow([s, x, f"{m:.4f}", f"{sd:.4f}", n])


def line_plot(out, name, data, xlabel, ylabel, title, logx=True, logy=False,
              ideal=None, xticks=None, ylim0=False):
    """One line per series with stddev error bars and a direct label at its end."""
    if not data:
        print(f"  {name}: no data, skipped")
        return
    fig, ax = plt.subplots(figsize=(6.8, 4.6))
    ends = []
    for s, style in SERIES.items():
        pts = data.get(s)
        if not pts:
            continue
        xs = list(pts)
        ms = [pts[x][0] for x in xs]
        sds = [pts[x][1] for x in xs]
        ax.errorbar(xs, ms, yerr=sds, color=style["color"], marker=style["marker"],
                    markeredgecolor=SURFACE, markeredgewidth=1.5, capsize=3,
                    elinewidth=1, label=style["label"], zorder=3)
        ends.append((ms[-1], xs[-1], style))
    if ideal is not None:
        ix, iy = ideal
        ax.plot(ix, iy, color=MUTED, linestyle="--", linewidth=1.2, label="Ideal", zorder=2)
    if logx:
        ax.set_xscale("log", base=2)
    if logy:
        ax.set_yscale("log")
    if xticks:
        ax.set_xticks(xticks)
        ax.set_xticklabels([str(t) for t in xticks])
    if ylim0 and not logy:
        ax.set_ylim(bottom=0)
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    ax.set_title(title, loc="left", fontsize=11, color=TEXT)
    handles, labels = ax.get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", ncol=3, fontsize=8.5,
               bbox_to_anchor=(0.5, 0.0))
    fig.tight_layout(rect=(0, 0.1, 0.83, 1))
    # direct labels at the right end of each line, at least 12 px apart
    fig.canvas.draw()
    to_px, to_data = ax.transData, ax.transData.inverted()
    placed = []
    for y, x, style in sorted(ends, key=lambda t: t[0]):
        px, py = to_px.transform((x, y))
        if placed and py - placed[-1] < 12:
            py = placed[-1] + 12
        placed.append(py)
        ax.annotate(style["label"].split(" (")[0], xy=to_data.transform((px, py)),
                    xytext=(7, 0), textcoords="offset points", va="center",
                    fontsize=8, color=TEXT, annotation_clip=False)
    for ext in ("png", "pdf"):
        fig.savefig(out / f"{name}.{ext}", dpi=200)
    plt.close(fig)
    write_csv(out / f"{name}.csv", data, xlabel.split(" (")[0].lower().replace(" ", "_"),
              ylabel.split(" (")[0].lower().replace(" ", "_"))
    print(f"  {name}: {', '.join(f'{s} {len(v)} points' for s, v in data.items())}")


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    out = Path(argv[0])
    out.mkdir(parents=True, exist_ok=True)
    exps = collect(argv[1:])
    print(f"{len(exps)} ok experiments from {len(argv) - 1} runs -> {out}")
    nodes = [1, 2, 4, 8]

    def is_(test, mode):
        return lambda e: e["test"] == test and e["mode"] == mode

    def cons_agg(e):
        return e["roles"].get("consumer", {}).get("agg")

    # weak scaling
    weak = group(exps, is_("weak", "seq"), "nodes", cons_agg)
    line_plot(out, "weak_seq_throughput", weak, "Nodes", "Aggregate read bandwidth (MB/s)",
              "Weak scaling: 4 pairs per node, write then read", logy=True, xticks=nodes)
    eff = {}
    for s, pts in weak.items():
        if 1 in pts:
            base = pts[1][0]
            eff[s] = {n: (m / n / base * 100, sd / n / base * 100, k)
                      for n, (m, sd, k) in pts.items()}
    line_plot(out, "weak_seq_efficiency", eff, "Nodes", "Weak-scaling efficiency (%)",
              "Weak scaling efficiency: per-node bandwidth vs 1 node", xticks=nodes,
              ideal=(nodes, [100] * len(nodes)), ylim0=True)
    weak_conc = group(exps, is_("weak", "conc"), "nodes", lambda e: e.get("e2e_MBps"))
    line_plot(out, "weak_conc_throughput", weak_conc, "Nodes",
              "End-to-end bandwidth (MB/s)",
              "Weak scaling, CAPIO streaming: 4 pairs per node", logy=True, xticks=nodes)

    # strong scaling
    strong = group(exps, is_("strong", "seq"), "nodes", total_time)
    t1 = [pts[1][0] for pts in strong.values() if 1 in pts]
    line_plot(out, "strong_seq_time", strong, "Nodes", "Time to write and read 16 GiB (s)",
              "Strong scaling: 32 pairs in total", logy=True, xticks=nodes)
    speed = {}
    for s, pts in strong.items():
        if 1 in pts:
            base = pts[1][0]
            speed[s] = {n: (base / m, base / m * (sd / m if m else 0), k)
                        for n, (m, sd, k) in pts.items()}
    line_plot(out, "strong_seq_speedup", speed, "Nodes", "Speedup (T1 / TN)",
              "Strong scaling speedup: 32 pairs in total", xticks=nodes,
              ideal=(nodes, nodes), ylim0=True)
    del t1

    # one node
    intra = group(exps, lambda e: e["test"] == "intranode_scaling", "pairs", cons_agg)
    line_plot(out, "intranode_scaling", intra, "Producer/consumer pairs",
              "Aggregate read bandwidth (MB/s)", "One node: more pairs", logy=True,
              xticks=[1, 2, 4, 8, 16, 32])
    chunk = group(exps, lambda e: e["test"] == "single_node" and e["mode"] == "seq"
                  and e["qd"] == "32" and e["pos"] == "0", "chunk", cons_agg)
    chunk = {s: {x // 1024: v for x, v in pts.items()} for s, pts in chunk.items()}
    line_plot(out, "single_node_chunk", chunk, "Chunk size (KiB)",
              "Read bandwidth (MB/s)", "One node, one pair: chunk size", ylim0=True,
              xticks=[4, 64, 256, 1024])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
