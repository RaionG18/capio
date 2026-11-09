#!/usr/bin/env python3
"""Small-file figures (run of jobs/12_small_files.slurm).

    plot_smallfiles.py OUT_DIR RUN_DIR

  smallfiles_time_per_file   time per file (ms) vs files per pair, 1 pair:
                             flat = each file costs the same; rising = the cost
                             of a file grows with the files already created
  smallfiles_pairs           files per second per node vs pairs per node
                             (250 files per pair)
  smallfiles_server_cpu      server CPU time per file (ms) vs files per server,
                             both tests (servers.csv): flat = linear total cost,
                             slope 1 on log-log = quadratic total cost

Time of one experiment: write then read = producer phase + consumer phase;
streaming = first producer start to last consumer end (as in plot_makespan.py).
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


def stats(v):
    return statistics.mean(v), statistics.stdev(v) if len(v) > 1 else 0.0, len(v)


def draw(out, name, data, xlabel, ylabel, title, logy=False, styles=None):
    fig, ax = plt.subplots(figsize=(6.8, 4.8))
    for key, st in (styles or STYLE).items():
        pts = data.get(key)
        if not pts:
            continue
        xs = sorted(pts)
        ax.errorbar(xs, [pts[x][0] for x in xs], yerr=[pts[x][1] for x in xs],
                    color=st["color"], marker=st["marker"], linestyle=st["ls"],
                    markeredgecolor=ps.SURFACE, markeredgewidth=1.5, capsize=3, elinewidth=1,
                    label=st["label"], zorder=3)
    ax.set_xscale("log", base=2)
    xs = sorted({x for pts in data.values() for x in pts})
    ax.set_xticks(xs)
    ax.set_xticklabels([str(x) for x in xs])
    if logy:
        ax.set_yscale("log")
        ax.yaxis.set_major_formatter(ps.FuncFormatter(lambda v, _: f"{v:,.0f}" if v >= 10
                                                      else f"{v:g}"))
        ax.yaxis.set_minor_formatter(ps.FuncFormatter(lambda v, _: ""))
    else:
        ax.set_ylim(bottom=0)
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    ax.set_title(title, loc="left", fontsize=11)
    h, l = ax.get_legend_handles_labels()
    fig.legend(h, l, loc="lower center", ncol=2, fontsize=8.5, bbox_to_anchor=(0.5, 0.0))
    fig.tight_layout(rect=(0, 0.15, 1, 1))
    for ext in ("png", "pdf"):
        fig.savefig(out / f"{name}.{ext}", dpi=200)
    plt.close(fig)
    with open(out / f"{name}.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["series", "mode", "x", "mean", "sd", "reps"])
        for (s, m), pts in sorted(data.items()):
            for x, (mu, sd, k) in sorted(pts.items()):
                w.writerow([s, m, x, f"{mu:.4f}", f"{sd:.4f}", k])
    print(f"  {name}: " + ", ".join(f"{s}/{m} {len(p)}" for (s, m), p in sorted(data.items())))


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    out = Path(argv[0])
    out.mkdir(parents=True, exist_ok=True)
    per_file = defaultdict(lambda: defaultdict(list))
    rate = defaultdict(lambda: defaultdict(list))
    for e in per_experiment(Path(argv[1])).values():
        key = (ps.series_of(e), e["mode"])
        if key not in STYLE:
            continue
        t = makespan(e)
        nf, pairs = int(e["nfiles"]), int(e["pairs"])
        if e["test"] == "files_per_pair":
            per_file[key][nf].append(t / nf * 1000)
        elif e["test"] == "pairs":
            rate[key][pairs].append(pairs * nf / t)  # files written and read per node per s
    draw(out, "smallfiles_time_per_file",
         {k: {x: stats(v) for x, v in d.items()} for k, d in per_file.items()},
         "Files per pair (4 KiB each)", "Time per file (ms)",
         "Small files, 1 pair across 2 nodes: time per file\n"
         "(flat = constant cost per file)")
    draw(out, "smallfiles_pairs",
         {k: {x: stats(v) for x, v in d.items()} for k, d in rate.items()},
         "Pairs per node (250 files of 4 KiB each)", "Files per second per node",
         "Small files across 2 nodes: throughput vs pairs per node", logy=True)
    # Server CPU per file: one point per server; x = files that server holds.
    cpu = defaultdict(lambda: defaultdict(list))
    with open(Path(argv[1]) / "servers.csv") as f:
        for r in csv.DictReader(f):
            if r["mode"] != "seq":
                continue
            n = int(r["nfiles"]) * int(r["pairs"])
            cpu[("capio", r["engine"])][n].append(
                (float(r["cpu_user_s"]) + float(r["cpu_sys_s"])) / n * 1000)
    cpu_style = {("capio", "posix"): dict(STYLE[("capio-posix", "seq")], label="CAPIO POSIX"),
                 ("capio", "io_uring"): dict(STYLE[("capio-io_uring", "seq")],
                                             label="CAPIO io_uring")}
    draw(out, "smallfiles_server_cpu",
         {k: {x: stats(v) for x, v in d.items()} for k, d in cpu.items()},
         "Files held by the server (4 KiB each)", "Server CPU per file (ms)",
         "Small files: CAPIO server CPU time per file\n"
         "(flat = constant cost per file; rising = cost grows with the file count)",
         logy=True, styles=cpu_style)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
