#!/usr/bin/env python3
"""Figures of the CAPIO paper's sizes (jobs/13_paper_sizes, jobs/14_paper_sizes_1k).

    plot_paper.py OUT_DIR RUN_1M [RUN_1K]

  paper_sizes   time to write and read 10 GiB per pair (makespan, as in
                plot_makespan.py), per file size; left 1 node, right 2 nodes
  paper_1k      write and read phase of 100 x 100 MiB in 1 KiB calls, 2 nodes

Bars are medians, whiskers min..max: these runs used shared slices, and both
the neighbours (a ~30% step on every configuration from rep 3 of the 1 MiB
run) and CAPIO's intermittent slow remote reads make the mean misleading.
"""
import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import plot_scaling as ps  # noqa: E402
from plot_makespan import STYLE, makespan  # noqa: E402
from summarize import per_experiment  # noqa: E402

plt = ps.plt
SIZES = [(100, 100 << 20, "100 × 100 MiB"), (10, 1 << 30, "10 × 1 GiB"),
         (1, 10 << 30, "1 × 10 GiB")]


def mmm(v):
    return statistics.median(v), min(v), max(v), len(v)


def bar(ax, x, w, st, m, lo, hi, label=None):
    ax.bar(x, m, w, color=st["color"], edgecolor=ps.SURFACE, linewidth=1.5,
           hatch=st.get("hatch"), label=label, zorder=3)
    ax.errorbar(x, m, yerr=[[m - lo], [hi - m]], color="#333333",
                capsize=3, elinewidth=1, linewidth=0, zorder=4)


def paper_sizes(out, run):
    t = defaultdict(list)  # (nodes, nfiles, key) -> seconds
    for e in per_experiment(run).values():
        key = (ps.series_of(e), e["mode"])
        ms = makespan(e)
        if key in STYLE and ms is not None:
            t[(int(e["nodes"]), int(e["nfiles"]), key)].append(ms)
    keys = [k for k in STYLE if any(k == kk for (_, _, kk) in t)]
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.8), sharey=False)
    rows = []
    for ax, nodes, title in zip(axes, (1, 2), ("1 node (consumer on the same node)",
                                               "2 nodes (consumer on the other node)")):
        present = [k for k in keys if any((nodes, nf, k) in t for nf, _, _ in SIZES)]
        w = 0.8 / len(present)
        for i, k in enumerate(present):
            for j, (nf, _, _) in enumerate(SIZES):
                v = t.get((nodes, nf, k))
                if not v:
                    continue
                m, lo, hi, n = mmm(v)
                bar(ax, j + (i - (len(present) - 1) / 2) * w, w, STYLE[k], m, lo, hi,
                    STYLE[k]["label"] if j == 0 else None)
                rows.append([nodes, nf, k[0], k[1], f"{m:.2f}", f"{lo:.2f}", f"{hi:.2f}", n])
        ax.set_xticks(range(len(SIZES)))
        ax.set_xticklabels([s[2] for s in SIZES])
        ax.set_ylim(bottom=0)
        ax.set_title(title, loc="left", fontsize=10.5)
        ax.set_ylabel("Seconds to write and read 10 GiB per pair")
    fig.suptitle("CAPIO paper sizes, 1 MiB calls: median time (whiskers min..max), lower is better",
                 x=0.01, ha="left", fontsize=11.5)
    h, l = [], []
    for ax in axes:
        for hh, ll in zip(*ax.get_legend_handles_labels()):
            if ll not in l:
                h.append(hh)
                l.append(ll)
    fig.legend(h, l, loc="lower center", ncol=4, fontsize=8.5, bbox_to_anchor=(0.5, 0.0))
    fig.tight_layout(rect=(0, 0.1, 1, 0.95))
    save(fig, out, "paper_sizes",
         ["nodes", "nfiles", "series", "mode", "median_s", "min_s", "max_s", "reps"], rows)


def paper_1k(out, run):
    ph = defaultdict(lambda: ([], []))
    for e in per_experiment(run).values():
        r = e["roles"]
        name = {("fs-lustre", "posix"): "Lustre", ("capio", "posix"): "CAPIO POSIX",
                ("capio", "io_uring"): "CAPIO io_uring"}[(e["config"], e["engine"])]
        if e["engine"] == "io_uring":
            name += ", no seeks" if str(e["pos"]) == "1" else ", with seeks"
        ph[name][0].append(r["producer"]["end"] - r["producer"]["start"])
        ph[name][1].append(r["consumer"]["end"] - r["consumer"]["start"])
    order = ["Lustre", "CAPIO POSIX", "CAPIO io_uring, no seeks", "CAPIO io_uring, with seeks"]
    order = [o for o in order if o in ph]
    fig, ax = plt.subplots(figsize=(7.5, 4.6))
    wst = dict(color="#2a78d6", hatch=None)
    rst = dict(color="#1baf7a", hatch=None)
    rows = []
    for i, name in enumerate(order):
        w, r = ph[name]
        wm, rm = statistics.median(w), statistics.median(r)
        ax.barh(i, wm, 0.6, color=wst["color"], edgecolor=ps.SURFACE, linewidth=1.5,
                label="Write phase" if i == 0 else None, zorder=3)
        ax.barh(i, rm, 0.6, left=wm, color=rst["color"], edgecolor=ps.SURFACE, linewidth=1.5,
                label="Read phase" if i == 0 else None, zorder=3)
        ax.text(wm + rm + 15, i, f"{wm + rm:,.0f} s", va="center", fontsize=9)
        rows.append([name, f"{wm:.1f}", f"{rm:.1f}", len(w)])
    ax.set_yticks(range(len(order)))
    ax.set_yticklabels(order)
    ax.invert_yaxis()
    ax.set_xlabel("Seconds (median)")
    ax.set_xlim(right=max(float(r[1]) + float(r[2]) for r in rows) * 1.15)
    ax.set_title("1 KiB calls, 100 × 100 MiB per pair, 2 nodes, write then read",
                 loc="left", fontsize=11)
    ax.legend(loc="upper right", fontsize=9)
    fig.tight_layout()
    save(fig, out, "paper_1k", ["config", "write_median_s", "read_median_s", "reps"], rows)


def save(fig, out, name, header, rows):
    for ext in ("png", "pdf"):
        fig.savefig(out / f"{name}.{ext}", dpi=200)
    plt.close(fig)
    with open(out / f"{name}.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)
    print(f"  {name}: {len(rows)} rows")


def main(argv):
    if len(argv) not in (2, 3):
        print(__doc__)
        return 2
    out = Path(argv[0])
    out.mkdir(parents=True, exist_ok=True)
    paper_sizes(out, Path(argv[1]))
    if len(argv) == 3:
        paper_1k(out, Path(argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
