# CAPIO-uring on Leonardo: measurement campaign and traces

Measurements of CAPIO with its io_uring engine ("CAPIO-uring") against CAPIO's
POSIX interception, Lustre and node-local disk, on CINECA Leonardo (DCGP
partition), November 2026. Fork `RaionG18/capio`, branch `test`; every run's
`meta.txt` records the exact commit.

## Contents

| Folder | What it holds |
|---|---|
| `plots/` | Figures as PNG and PDF, with the CSV of the plotted numbers (mean and stddev over repetitions) |
| `campaign/` | The measurement runs, whole nodes (112 cores each) |
| `traces/` | CAPIO's internal traces (Perfetto format) of 10 representative cases |
| `functional_tests/` | Shared-slice runs that validated the setup before the campaign (smoke, stress, preflight, InfiniBand check) |
| `superseded/` | The first cross-node run, measured over the wrong network (see Caveats); kept for the record, not for comparison |
| `slurm/` | Slurm output of every job |
| `summary_all.csv` | One row per configuration over all campaign runs |

Each run directory `<jobid>_<test>/` contains `meta.txt` (commit, nodes, kernel,
modules), `experiments.csv` (one row per experiment, status, wall time),
`results.csv` (one row per producer/consumer process), `summary.txt` /
`summary.csv` (mean ± stddev), `job.log`, and `logs/<experiment>/node<k>/` (CAPIO
server log, node log, output of every process).

## Setup

- **Nodes:** Leonardo DCGP, 112 cores and 512 GB per node, exclusive for the
  campaign. Kernel 4.18 (RHEL 8.8), which has **no io_uring**: there,
  CAPIO-uring runs CAPIO's own ring with no kernel ring behind it (requests on
  CAPIO files are served by CAPIO; the benchmark only touches CAPIO files).
- **Benchmark:** `one_to_one` (fork, `examples/io_uring/`). Each producer/consumer
  pair moves 64 files × 8 MiB = 512 MiB; the producer writes, the consumer reads
  and verifies every file (checksums). All runs verified all files.
- **Configurations:** Lustre scratch (POSIX), node-local disk (POSIX, `/tmp`),
  CAPIO with POSIX interception, CAPIO with io_uring (256 KiB chunks, queue
  depth 32 unless stated).
- **Modes:** `seq` = all producers, then all consumers; `conc` = streaming,
  consumers start first and read while producers write (CAPIO only).
- **Placement:** `local` = consumer on the producer's node; `cross` = consumer on
  the next node, so data crosses the network. CAPIO servers talk over TCP on
  InfiniBand (`ib0`).
- **Metrics:** aggregate bandwidth = total bytes / (last process end − first
  process start) over all processes of a role and all nodes. Streaming:
  end-to-end bandwidth = bytes / (last consumer end − first producer start).
  Repetitions: 3 (scaling), 5 (single node, cross-node).

## Campaign runs

| Run | Study | Nodes | Experiments |
|---|---|---|---|
| `63952926_single_node` | configurations, chunk size, queue depth | 1 | 72 |
| `63952927_intranode_scaling` | 1–32 pairs on one node | 1 | 72 |
| `63952928_cross_node` (in `superseded/`) | local and cross placement, 1 and 8 pairs; its local-placement rows are valid | 2 | 90 |
| `63991233_cross_node_ib` | cross placement redone over InfiniBand | 2 | 50 |
| `63952931_scaling_n1` … `63952935_scaling_n8` | weak (4 pairs per node) and strong (32 pairs in total) scaling | 1, 2, 4, 8 | 27 each |

All experiments succeeded. Cost of the campaign: 305 core-hours (account
`ict26_mhpc`), plus about 20 core-hours of functional tests and traces.

## Main results

**One node, one pair, read bandwidth (MB/s):**

| Chunk | CAPIO io_uring | CAPIO POSIX | Node-local disk |
|---|---|---|---|
| 4 KiB | 76 | 946 | 2,239 |
| 256 KiB | 820 | 995 | 2,433 |
| 1 MiB | 943 | 1,025 | 2,063 |

Without a kernel ring, CAPIO-uring has a fixed cost per request: 12× slower
than POSIX interception at 4 KiB, within 10% from 256 KiB up.

**One node, more pairs (aggregate read MB/s):** CAPIO peaks at 4 pairs (io_uring
1,767, POSIX 1,936) and falls to about 460 at 32 pairs, both engines alike: the
single CAPIO server per node is the bottleneck. Lustre and node-local disk keep
scaling (42,000 and 50,000 MB/s at 32 pairs).

**Two nodes, consumer on the other node (MB/s):**

| Case | CAPIO io_uring | CAPIO POSIX | Lustre |
|---|---|---|---|
| 1 pair, write then read | 500 | 523 | 776 |
| 8 pairs, write then read | 1,533 | 1,622 | 5,023 |
| 1 pair, streaming, end to end | 482 | 484 | — |
| 8 pairs, streaming, end to end | 1,001 | 1,027 | — |

**Strong scaling** (`strong_seq_time`, `strong_seq_speedup`): 16 GiB written and
read by 32 pairs. CAPIO: 44–47 s on 1 node, 7–8 s on 8 nodes (speedup 6.1 POSIX,
5.5 io_uring; ideal 8). Lustre: 2–4 s at every node count (no speedup).

**Weak scaling** (`weak_seq_*`, `weak_conc_throughput`): 4 pairs per node. At 8
nodes CAPIO reads 5,500–5,600 MB/s in aggregate, close to Lustre's 5,800. From
2 nodes on, CAPIO keeps 35–44% of its 1-node per-node bandwidth, Lustre 9–22%.
Streaming end to end grows from about 1,100 MB/s (1 node) to 2,560 (POSIX) and
2,850 (io_uring) MB/s (8 nodes).

## Caveats

1. **No kernel io_uring on Leonardo.** The io_uring results measure CAPIO's
   userspace ring only; a comparison with the kernel's io_uring needs a newer
   kernel.
2. **At 1 node, "cross" is the same node.** In the scaling runs the consumer reads
   on the next node, which at 1 node is its own: CAPIO serves from local memory
   and Lustre reads from the client page cache. The 1-node points are therefore
   faster than any cross-node point, and weak-scaling efficiency drops from 1 to
   2 nodes for this reason. Node-local disk is always same-node.
3. **Lustre timings include client caching** (written data is buffered by the
   Lustre client), which flatters the short write-then-read runs.
4. **Run `63952928` (superseded) used the wrong network.** The node hostnames
   resolve to the 1 Gb/s management network; CAPIO servers connected through it,
   capping cross-node CAPIO at about 114 MB/s per direction. Fixed by advertising
   the InfiniBand address (`CAPIO_MTCL_ADVERTISE_HOST`, fork commit `9be122b`)
   and re-measured in `63991233_cross_node_ib`. The local-placement rows of
   `63952928` are unaffected.
5. Shared-slice functional tests (`functional_tests/`) are not measurements.

## Traces (`traces/63992951_capio_logs/`)

CAPIO's own trace of every request, in the server and in each client process,
for ten cases with small files (4 files × 1 MiB per pair), both engines:

| Experiments | Case |
|---|---|
| e001, e006 | one pair, chunk 4 KiB (where io_uring is 12× slower) |
| e002, e007 | one pair, chunk 256 KiB |
| e003, e008 | one pair, streaming on one node |
| e004, e009 | one pair, streaming across two nodes (remote reads) |
| e005, e010 | four pairs on one node (server contention) |

Each `logs/<experiment>/node<k>/capio_logs.tar.gz` holds one
`.perfetto-trace` file per process (`server_<pid>`, `posix_<pid>` for each
client). Open them in <https://ui.perfetto.dev> ("Open trace file"; the file is
processed locally in the browser).

These come from a Debug build with logging enabled (`build.sh --logs`), so their
timings are slower than the campaign's and are **not** measurements: they show
what CAPIO does, not how fast.

## Reproducing

On a Leonardo login node, in the fork's `test` branch: `./leonardo/build.sh`,
then `./leonardo/campaign.sh` (see `leonardo/README.md`). Plots:
`python3 leonardo/analysis/plot_scaling.py OUT_DIR RUN_DIR...` (needs
matplotlib). This package: `leonardo/analysis/make_package.sh`.
