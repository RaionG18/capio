#!/usr/bin/env python3
"""Seek test figure (RQ2: where does the io_uring cost come from?).

    plot_seek.py OUT_DIR SEEK_RUN_DIR

SEEK_RUN_DIR is a run of jobs/05_seek_test.slurm. Two panels, write and read
aggregate bandwidth per chunk size, for CAPIO POSIX, CAPIO io_uring with
positioned SQEs (two capio_lseek round trips each) and CAPIO io_uring with
off = -1 (no seek). Writes seek_test.png/.pdf/.csv.
"""
import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import plot_scaling as ps  # noqa: E402
from summarize import per_experiment  # noqa: E402

plt = ps.plt
BARS = [  # (engine, pos) -> style; color = engine as in every figure, texture = no seek
    (("posix", "0"), dict(label="CAPIO POSIX", color="#1baf7a", hatch=None)),
    (("io_uring", "0"), dict(label="CAPIO io_uring, positioned (with seeks)",
                             color="#eda100", hatch=None)),
    (("io_uring", "1"), dict(label="CAPIO io_uring, off = -1 (no seek)", color="#eda100",
                             hatch="///")),
]


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    out = Path(argv[0])
    out.mkdir(parents=True, exist_ok=True)
    acc = defaultdict(list)
    for e in per_experiment(Path(argv[1])).values():
        for role in ("producer", "consumer"):
            acc[(role, int(e["chunk"]) // 1024, e["engine"], e["pos"])].append(
                e["roles"][role]["agg"])
    chunks = sorted({k[1] for k in acc})
    fig, axes = plt.subplots(1, 2, figsize=(8.6, 4.4), sharey=True)
    width = 0.8 / len(BARS)
    rows = []
    for ax, role, title in zip(axes, ("producer", "consumer"), ("Write", "Read")):
        for ci, chunk in enumerate(chunks):
            for bi, (key, st) in enumerate(BARS):
                v = acc.get((role, chunk, *key))
                if not v:
                    continue
                mu = statistics.mean(v)
                sd = statistics.stdev(v) if len(v) > 1 else 0.0
                rows.append([role, chunk, key[0], key[1], f"{mu:.2f}", f"{sd:.2f}", len(v)])
                x = ci + (bi - (len(BARS) - 1) / 2) * width
                ax.bar(x, mu, width * 0.88, yerr=sd, color=st["color"], hatch=st["hatch"],
                       edgecolor=ps.SURFACE, linewidth=0, capsize=2, ecolor=ps.MUTED, zorder=3,
                       label=st["label"] if ci == 0 else None)
                ax.annotate(f"{mu:.0f}", (x, mu + sd), xytext=(0, 2), textcoords="offset points",
                            ha="center", va="bottom", fontsize=7, color=ps.TEXT)
        ax.set_xticks(range(len(chunks)))
        ax.set_xticklabels([f"{c} KiB" for c in chunks])
        ax.set_xlabel("Chunk size")
        ax.set_title(title, loc="left", fontsize=11)
        ax.grid(axis="x", visible=False)
    axes[0].set_ylabel("Aggregate bandwidth (MB/s)")
    fig.suptitle("io_uring with and without seeks, against POSIX (mean of 5 runs, whiskers: stddev)",
                 x=0.02, ha="left", fontsize=12)
    h, l = axes[0].get_legend_handles_labels()
    fig.legend(h, l, loc="lower center", ncol=2, fontsize=8.5, bbox_to_anchor=(0.5, 0.0))
    fig.tight_layout(rect=(0, 0.12, 1, 0.95))
    for ext in ("png", "pdf"):
        fig.savefig(out / f"seek_test.{ext}", dpi=200)
    plt.close(fig)
    with open(out / "seek_test.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["role", "chunk_KiB", "engine", "pos", "MBps_mean", "MBps_sd", "reps"])
        w.writerows(rows)
    print(f"  seek_test: {len(rows)} bars")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
