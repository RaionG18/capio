#!/usr/bin/env python3
"""Makespan figures (RQ3: does CAPIO's streaming still pay off with io_uring?).

    plot_makespan.py OUT_DIR RUN_DIR [RUN_DIR ...]

Makespan of one experiment: streaming (conc) = last consumer end - first
producer start; write then read (seq) = producer phase + consumer phase, without
the harness barrier between the two. Writes PNG, PDF and CSV for:

  makespan_weak       weak scaling (4 pairs per node): makespan vs nodes, CAPIO
                      write-then-read and streaming, Lustre write-then-read
  streaming_gain      makespan(write then read) / makespan(streaming), per engine;
                      1 = no gain, 2 = write and read fully overlapped (the bound
                      for a pipeline that only does I/O)
  makespan_cases      one node / two nodes (1 and 8 pairs): makespan per
                      configuration, as grouped bars
"""
import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import plot_scaling as ps  # noqa: E402  (style, colors, helpers)
from summarize import per_experiment  # noqa: E402

plt = ps.plt

# Color = configuration (as in every other figure); line/hatch = mode.
STYLE = {
    ("fs-lustre", "seq"): dict(label="Lustre, write then read", color="#2a78d6", marker="o",
                               ls="-", hatch=None),
    ("fs-tmp", "seq"): dict(label="Node-local disk, write then read", color="#eb6834",
                            marker="s", ls="-", hatch=None),
    ("capio-posix", "seq"): dict(label="CAPIO POSIX, write then read", color="#1baf7a",
                                 marker="^", ls="--", hatch=None),
    ("capio-posix", "conc"): dict(label="CAPIO POSIX, streaming", color="#1baf7a",
                                  marker="^", ls="-", hatch="///"),
    ("capio-io_uring", "seq"): dict(label="CAPIO io_uring, write then read", color="#eda100",
                                    marker="D", ls="--", hatch=None),
    ("capio-io_uring", "conc"): dict(label="CAPIO io_uring, streaming", color="#eda100",
                                     marker="D", ls="-", hatch="///"),
}


def makespan(e):
    """Streaming: first producer start -> last consumer end. Write then read: producer
    phase + consumer phase, without the harness barrier between them (idle time
    spent polling a file on Lustre, 0.05-4 s, which is not part of the pipeline)."""
    r = e["roles"]
    if "producer" not in r or "consumer" not in r:
        return None
    if e["mode"] == "seq":
        return ((r["producer"]["end"] - r["producer"]["start"])
                + (r["consumer"]["end"] - r["consumer"]["start"]))
    return r["consumer"]["end"] - r["producer"]["start"]


def stats(v):
    return (statistics.mean(v), statistics.stdev(v) if len(v) > 1 else 0.0, len(v))


def collect(runs):
    exps = []
    for run in runs:
        exps += per_experiment(Path(run)).values()
    return exps


def save(fig, out, name):
    for ext in ("png", "pdf"):
        fig.savefig(out / f"{name}.{ext}", dpi=200)
    plt.close(fig)


def weak(out, exps):
    acc = defaultdict(list)
    for e in exps:
        if e["test"] == "weak" and (ps.series_of(e), e["mode"]) in STYLE:
            acc[(ps.series_of(e), e["mode"], int(e["nodes"]))].append(makespan(e))
    data = defaultdict(dict)
    for (s, m, n), v in acc.items():
        data[(s, m)][n] = stats(v)
    if not data:
        print("  makespan_weak: no data")
        return
    nodes = [1, 2, 4, 8]
    fig, ax = plt.subplots(figsize=(6.8, 4.8))
    for key, st in STYLE.items():
        if key not in data or key[0] == "fs-tmp":
            continue  # node-local disk never crosses nodes: not a pipeline between nodes
        pts = data[key]
        xs = sorted(pts)
        ax.errorbar(xs, [pts[x][0] for x in xs], yerr=[pts[x][1] for x in xs], color=st["color"],
                    marker=st["marker"], linestyle=st["ls"], markeredgecolor=ps.SURFACE,
                    markeredgewidth=1.5, capsize=3, elinewidth=1, label=st["label"], zorder=3)
    ax.set_xscale("log", base=2)
    ax.set_xticks(nodes)
    ax.set_xticklabels([str(n) for n in nodes])
    ax.set_ylim(bottom=0)
    ax.set_xlabel("Nodes")
    ax.set_ylabel("Makespan (s)")
    ax.set_title("Producer -> consumer makespan, weak scaling (4 pairs per node)\n"
                 "(consumer on the next node; at 1 node, the same node)", loc="left",
                 fontsize=11)
    h, l = ax.get_legend_handles_labels()
    fig.legend(h, l, loc="lower center", ncol=2, fontsize=8.5, bbox_to_anchor=(0.5, 0.0))
    fig.tight_layout(rect=(0, 0.15, 1, 1))
    save(fig, out, "makespan_weak")
    with open(out / "makespan_weak.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["series", "mode", "nodes", "makespan_s_mean", "makespan_s_sd", "reps"])
        for (s, m), pts in sorted(data.items()):
            for n, (mu, sd, k) in sorted(pts.items()):
                w.writerow([s, m, n, f"{mu:.4f}", f"{sd:.4f}", k])
    print("  makespan_weak: " + ", ".join(f"{s}/{m}" for s, m in sorted(data)))

    # streaming gain = seq / conc, per engine
    gain = {}
    for eng in ("capio-posix", "capio-io_uring"):
        if (eng, "seq") in data and (eng, "conc") in data:
            gain[eng] = {}
            for n in sorted(set(data[(eng, "seq")]) & set(data[(eng, "conc")])):
                s_mu, s_sd, k = data[(eng, "seq")][n]
                c_mu, c_sd, _ = data[(eng, "conc")][n]
                g = s_mu / c_mu
                gain[eng][n] = (g, g * ((s_sd / s_mu) ** 2 + (c_sd / c_mu) ** 2) ** 0.5, k)
    ps.line_plot(out, "streaming_gain", gain, "Nodes",
                 "Streaming gain (makespan ratio)",
                 "Streaming gain: makespan(write then read) / makespan(streaming)\n"
                 "(1 = no gain; 2 = write and read fully overlapped)",
                 xticks=nodes, ideal=(nodes, [2] * len(nodes)), ylim0=True,
                 ideal_label="Full overlap (bound for pure I/O)")


def cases(out, exps):
    """Grouped bars: makespan per configuration in three placements."""
    groups = [
        ("1 node, 1 pair", lambda e: e["test"] == "single_node" and e["chunk"] == "262144"
         and e["qd"] == "32" and e["pos"] == "0"),
        ("2 nodes, 1 pair\n(consumer remote)", lambda e: e["test"] == "cross_node_ib"
         and e["pairs"] == "1"),
        ("2 nodes, 8 pairs\n(consumer remote)", lambda e: e["test"] == "cross_node_ib"
         and e["pairs"] == "8"),
    ]
    keys = [k for k in STYLE if k[0] != "fs-tmp"]
    table = {}
    for gname, sel in groups:
        acc = defaultdict(list)
        for e in exps:
            k = (ps.series_of(e), e["mode"])
            if sel(e) and k in STYLE:
                acc[k].append(makespan(e))
        table[gname] = {k: stats(v) for k, v in acc.items()}
    fig, ax = plt.subplots(figsize=(7.4, 4.8))
    width = 0.8 / len(keys)
    for gi, (gname, _) in enumerate(groups):
        for ki, k in enumerate(keys):
            if k not in table[gname]:
                continue
            mu, sd, _ = table[gname][k]
            st = STYLE[k]
            x = gi + (ki - (len(keys) - 1) / 2) * width
            ax.bar(x, mu, width * 0.88, yerr=sd, color=st["color"], hatch=st["hatch"],
                   edgecolor=ps.SURFACE, linewidth=0, capsize=2, ecolor=ps.MUTED,
                   label=st["label"] if gi == 0 else None, zorder=3)
            ax.annotate(f"{mu:.1f}", (x, mu + sd), xytext=(0, 2), textcoords="offset points",
                        ha="center", va="bottom", fontsize=7, color=ps.TEXT)
    ax.set_xticks(range(len(groups)))
    ax.set_xticklabels([g for g, _ in groups])
    ax.set_ylabel("Makespan (s)")
    ax.grid(axis="x", visible=False)
    ax.set_title("Producer -> consumer makespan: 64 files x 8 MiB per pair", loc="left",
                 fontsize=11)
    h, l = ax.get_legend_handles_labels()
    fig.legend(h, l, loc="lower center", ncol=2, fontsize=8.5, bbox_to_anchor=(0.5, 0.0))
    fig.tight_layout(rect=(0, 0.15, 1, 1))
    save(fig, out, "makespan_cases")
    with open(out / "makespan_cases.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["case", "series", "mode", "makespan_s_mean", "makespan_s_sd", "reps"])
        for gname, _ in groups:
            for (s, m), (mu, sd, k) in sorted(table[gname].items()):
                w.writerow([gname.replace("\n", " "), s, m, f"{mu:.4f}", f"{sd:.4f}", k])
    print("  makespan_cases: " + "; ".join(f"{g.split(chr(10))[0]} {len(t)} configs"
                                           for g, t in table.items()))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    out = Path(argv[0])
    out.mkdir(parents=True, exist_ok=True)
    exps = collect(argv[1:])
    print(f"{len(exps)} ok experiments -> {out}")
    weak(out, exps)
    cases(out, exps)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
