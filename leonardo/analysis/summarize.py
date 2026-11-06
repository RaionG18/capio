#!/usr/bin/env python3
"""Summarise Leonardo benchmark runs.

    summarize.py RUN_DIR [RUN_DIR ...]      print tables, write RUN_DIR/summary.csv
    summarize.py --all RESULTS_ROOT         every run under RESULTS_ROOT, plus
                                            RESULTS_ROOT/summary_all.csv

Per experiment (one repetition) and role:
  agg_MBps  = total bytes / (last process end - first process start), all nodes
  proc_MBps = mean over processes of the program's own MB/s (I/O loop only)
For mode=conc the consumers start first and wait for the data, so their span is
not a bandwidth: the pipeline is measured instead, e2e_s = last consumer end -
first producer start, and e2e_MBps = bytes / e2e_s.
Repetitions of the same parameters are reported as mean +- stddev.
"""
import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path

KEY = ["test", "config", "engine", "nodes", "pairs", "placement", "mode",
       "nfiles", "fsize", "chunk", "qd", "pos"]


def size(n):
    n = int(n)
    for unit in ("G", "M", "K"):
        scale = {"G": 1 << 30, "M": 1 << 20, "K": 1 << 10}[unit]
        if n >= scale and n % scale == 0:
            return f"{n // scale}{unit}"
    return str(n)


def per_experiment(run_dir):
    """{exp: dict(params..., roles={role: stats}, e2e_s)} for experiments with status ok."""
    status = {}
    with open(run_dir / "experiments.csv") as f:
        for row in csv.DictReader(f):
            status[row["exp"]] = row
    rows = defaultdict(list)
    with open(run_dir / "results.csv") as f:
        for row in csv.DictReader(f):
            rows[row["exp"]].append(row)
    out = {}
    for exp, procs in rows.items():
        if status.get(exp, {}).get("status") != "ok":
            continue
        info = {k: procs[0][k] for k in KEY + ["rep", "run_id"]}
        info["roles"] = {}
        for role in ("producer", "consumer"):
            ps = [p for p in procs if p["role"] == role]
            if not ps:
                continue
            start = min(float(p["t_start"]) for p in ps)
            end = max(float(p["t_end"]) for p in ps)
            total = sum(int(p["bytes"] or 0) for p in ps)
            info["roles"][role] = {
                "agg": total / 1e6 / (end - start) if end > start else 0.0,
                "proc": statistics.mean(float(p["MBps"] or 0) for p in ps),
                "start": start, "end": end, "bytes": total,
            }
        r = info["roles"]
        if info["mode"] == "conc" and "producer" in r and "consumer" in r:
            info["e2e_s"] = r["consumer"]["end"] - r["producer"]["start"]
            info["e2e_MBps"] = r["consumer"]["bytes"] / 1e6 / info["e2e_s"]
            del r["consumer"]["agg"]
        verified = [int(p["verified"] or 0) for p in procs if p["role"] == "consumer"]
        info["verified"] = f"{sum(verified)}/{len(verified) * int(info['nfiles'])}"
        out[exp] = info
    return out


def mean_sd(values, digits=1):
    if not values:
        return ""
    if len(values) == 1:
        return f"{values[0]:.{digits}f}"
    return f"{statistics.mean(values):.{digits}f} ± {statistics.stdev(values):.{digits}f}"


def summarise(experiments):
    groups = defaultdict(list)
    for info in experiments:
        groups[tuple(info[k] for k in KEY)].append(info)
    table = []
    for key, reps in sorted(groups.items(), key=lambda kv: [
            (int(v) if v.isdigit() else v) for v in kv[0]]):
        params = dict(zip(KEY, key))

        def vals(role, field):
            return [r["roles"][role][field] for r in reps if field in r["roles"].get(role, {})]
        table.append({
            **params,
            "reps": len(reps),
            "prod_agg_MBps": mean_sd(vals("producer", "agg")),
            "cons_agg_MBps": mean_sd(vals("consumer", "agg")),
            "prod_proc_MBps": mean_sd(vals("producer", "proc")),
            "cons_proc_MBps": mean_sd(vals("consumer", "proc")),
            "e2e_s": mean_sd([r["e2e_s"] for r in reps if "e2e_s" in r], 3),
            "e2e_MBps": mean_sd([r["e2e_MBps"] for r in reps if "e2e_MBps" in r]),
            "verified": " ".join(sorted({r["verified"] for r in reps})),
        })
    return table


def print_table(table):
    cols = ["test", "config", "engine", "nodes", "pairs", "placement", "mode", "chunk", "qd",
            "pos", "reps", "prod_agg_MBps", "cons_agg_MBps", "e2e_s", "e2e_MBps", "verified"]
    rows = [[size(t[c]) if c == "chunk" else str(t[c]) for c in cols] for t in table]
    widths = [max(len(c), *(len(r[i]) for r in rows)) if rows else len(c)
              for i, c in enumerate(cols)]
    print("  ".join(c.ljust(w) for c, w in zip(cols, widths)))
    for r in rows:
        print("  ".join(v.ljust(w) for v, w in zip(r, widths)))


def write_csv(path, table):
    if not table:
        return
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(table[0].keys()))
        w.writeheader()
        w.writerows(table)


def main(argv):
    if len(argv) >= 2 and argv[0] == "--all":
        root = Path(argv[1])
        runs = sorted(p for p in root.iterdir() if (p / "results.csv").exists())
        everything = []
        for run in runs:
            everything += per_experiment(run).values()
        table = summarise(everything)
        print_table(table)
        write_csv(root / "summary_all.csv", table)
        return 0
    for arg in argv:
        run = Path(arg)
        table = summarise(per_experiment(run).values())
        print(f"== {run.name}")
        print_table(table)
        write_csv(run / "summary.csv", table)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]) if len(sys.argv) > 1 else print(__doc__) or 2)
