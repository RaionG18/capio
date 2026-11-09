#!/usr/bin/env python3
"""Count CAPIO client -> server round trips in CAPIO's Perfetto traces.

    trace_roundtrips.py TRACE_RUN_DIR [EXP ...]

TRACE_RUN_DIR is a run of jobs/40_capio_logs.slurm (logs/<exp>/node<k>/
capio_logs.tar.gz). For every client trace (posix_<pid>.perfetto-trace) of the
given experiments (default: all) it prints the I/O calls of the program and the
requests the client sent to its CAPIO server: seek_request, write_request,
read_request (outermost calls only), and round trips per MiB moved.

Only counts are reliable: the traces come from a Debug build with tracing on,
so their durations are not measurements.

Needs the `perfetto` Python package (pip install perfetto); it downloads
Perfetto's trace_processor on first use.
"""
import re
import sys
import tarfile
import tempfile
from pathlib import Path

from perfetto.trace_processor import TraceProcessor

REQUESTS = ("seek_request", "write_request", "read_request")


def counts(trace):
    tp = TraceProcessor(trace=str(trace))
    try:
        names = "','".join(REQUESTS + ("capio_write", "capio_read"))
        return {r.name: r.n for r in tp.query(f"""
            select s.name, count(*) n from slice s
            where s.name in ('{names}')
              and not exists (select 1 from ancestor_slice(s.id) a where a.name = s.name)
            group by s.name""")}
    finally:
        tp.close()


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    run = Path(argv[0])
    wanted = set(argv[1:])
    rows = []
    meta = {}
    with open(run / "experiments.csv") as f:
        header = f.readline().strip().split(",")
        for line in f:
            r = dict(zip(header, line.strip().split(",")))
            meta[r["exp"]] = r
    for exp_dir in sorted((run / "logs").iterdir()):
        exp = exp_dir.name
        if wanted and not any(exp.startswith(w) for w in wanted):
            continue
        m = meta.get(exp, {})
        mib = int(m.get("nfiles", 0)) * int(m.get("fsize", 0)) / 2**20 or 1
        for archive in sorted(exp_dir.glob("node*/capio_logs.tar.gz")):
            with tempfile.TemporaryDirectory() as tmp:
                with tarfile.open(archive) as t:
                    t.extractall(tmp, filter="data")
                for trace in sorted(Path(tmp).rglob("posix_*.perfetto-trace")):
                    c = counts(trace)
                    role = "producer" if c.get("write_request", 0) >= c.get("read_request", 0) \
                        else "consumer"
                    io = c.get("capio_write" if role == "producer" else "capio_read", 0)
                    rt = sum(c.get(k, 0) for k in REQUESTS)
                    rows.append((exp[:4], m.get("engine", "?"), m.get("chunk", "?"), role, io,
                                 *(c.get(k, 0) for k in REQUESTS), rt / mib))
    print(f"{'exp':5} {'engine':9} {'chunk':>8} {'role':9} {'io_calls':>8} {'seek':>6} "
          f"{'write':>6} {'read':>6} {'RT/MiB':>7}")
    for r in rows:
        print(f"{r[0]:5} {r[1]:9} {r[2]:>8} {r[3]:9} {r[4]:>8} {r[5]:>6} {r[6]:>6} {r[7]:>6} "
              f"{r[8]:>7.0f}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
