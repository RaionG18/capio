# CAPIO io_uring tests on Leonardo

Scripts to build CAPIO on Leonardo (CINECA, DCGP partition) and measure
CAPIO-uring against CAPIO-posix and the filesystems, on one node and across
nodes.

## Leonardo has no io_uring

Leonardo runs RHEL 8.8 with kernel 4.18 on login and compute nodes, and
`io_uring_setup` returns `ENOSYS`. This has two consequences:

- **FS-uring cannot run.** The benchmark's io_uring engine fails without CAPIO.
  Those experiments are skipped and marked `skipped` in `experiments.csv`.
- **CAPIO-uring runs without a kernel ring.** CAPIO normally forwards SQEs for
  non-CAPIO files to a hidden kernel ring. When the kernel has no io_uring
  (`ENOSYS`, or `EPERM` from `kernel.io_uring_disabled`), setup still succeeds:
  SQEs on CAPIO files are served as usual, `NOP` and `CLOSE` work, and every
  other SQE completes with `-ENOSYS`. Waits use a condition variable instead of
  the kernel ring. EXT_ARG timeouts are honoured. As a result, an unmodified
  liburing program does I/O on CAPIO files on a kernel without io_uring.

The system `<linux/io_uring.h>` is too old to compile CAPIO's io_uring code,
so `build.sh` compiles CAPIO against the header that ships with liburing 2.14.

`tools/no_io_uring.c` reproduces the Leonardo condition on any machine. It
installs a seccomp filter so that the io_uring syscalls fail with `ENOSYS`.

## Usage (on a login node)

```bash
git clone -b test https://github.com/RaionG18/capio.git && cd capio
./leonardo/build.sh                          # liburing + CAPIO + benchmark (login node: needs internet)
./leonardo/submit.sh leonardo/jobs/00_smoke.slurm
./leonardo/submit.sh leonardo/jobs/01_smoke_2nodes.slurm
./leonardo/submit.sh leonardo/jobs/03_preflight_8nodes.slurm  # every campaign shape, small files
# after the smoke and preflight tests pass: the measurement jobs, chained (one at a time)
./leonardo/campaign.sh
python3 leonardo/analysis/summarize.py --all leonardo/results   # every run, one table
```

## Fair use (shared account `ict26_mhpc`)

- **Functional tests use a shared slice** (8 cores, 32 GB): smoke, stress and
  preflight (8 nodes × 8 cores, every shape the campaign uses, small files).
- **Only measurement jobs take whole nodes**, so that other users' jobs do not
  disturb the timings. Billing is per allocated core: one DCGP node is 112
  core-hours per hour of use.
- **`campaign.sh` chains the jobs** (`afterok`): one allocation at a time,
  8 nodes at most, and the account's queue is never flooded. A job that aborts
  or hits its walltime stops the chain; cancel the pending rest with `scancel`.
- **A stuck experiment costs minutes, not the job.** Each experiment's time
  limit follows from the data per node (180 s + 4× the time at 25 MB/s per
  node, at most 1200 s; `EXP_TIMEOUT`/`PROC_TIMEOUT` override it), and a job
  stops after 2 failed experiments in a row (`MAX_CONSECUTIVE_FAILS`); the
  rest are recorded as `aborted`.
- **Walltimes are tight upper bounds.** The whole campaign is at most ≈1,500
  core-hours and about half of that in practice (account budget at submission:
  16,615 core-hours/month, ≈54,000 left until 31 Dec 2026). Check with
  `saldo -b` before submitting.
- **Builds run on the login node with `-j4`**, since FetchContent needs the
  internet.
- **Barriers poll Lustre every 0.5 s.** Benchmark data are deleted after each
  experiment, and the run's scratch directory when the job ends.

## Output: one directory per run

`leonardo/results/<jobid>_<test>/` (ignored by git; finished runs are copied to
`leonardo/runs/` and committed):

| File | Content |
|---|---|
| `job.log` | everything the job printed |
| `meta.txt` | commit, nodes, kernel, modules |
| `experiments.csv` | one row per experiment: parameters, `ok`/`failed`/`skipped`/`aborted`, wall time, CAPIO files found on disk |
| `results.csv` | one row per producer/consumer process: host, start/end time, MB/s, files verified, exit code |
| `summary.txt`, `summary.csv` | mean ± stddev over repetitions |
| `logs/<exp>/node<k>/` | CAPIO server log, node log, output of every process |

Slurm's own output goes to `leonardo/results/slurm/<jobid>_<jobname>.out`.

## What one experiment is

Workload: `examples/io_uring/one_to_one` (gains `-t TAG`, a file-name prefix, so
that many pairs can share a directory). A pair is one producer that writes
`nfiles` files of `fsize` bytes in `chunk`-sized writes, and one consumer that
reads them back and checks every byte. The io_uring engine keeps `qd` SQEs in
flight.

`run_exp` (in `lib/common.sh`) launches one `srun` task per node
(`lib/node_run.sh`). Each node:

1. starts its CAPIO server (`capio` only): MTCL over TCP, discovery through
   token files on Lustre, `CAPIO_DIR` on Lustre, as in CAPIO's multinode
   integration test. It waits until the servers are connected in a full mesh;
2. runs `pairs` producers. Their consumers run on the same node
   (`placement=local`) or on the next node (`placement=cross`, rank+1 mod N);
3. `mode=seq`: all producers finish, then all consumers start.
   `mode=conc`: consumers start first and wait for files that do not exist yet
   (CAPIO streaming, `on_close` + `no_update`).
4. Barriers between phases are files on Lustre. Every experiment uses a fresh
   server, workflow name, port and directory.

| config | where the data goes |
|---|---|
| `fs-lustre` | `$CINECA_SCRATCH` (Lustre, shared) |
| `fs-tmp` | node-local NVMe (`--gres=tmpfs`), so only `placement=local` |
| `capio` | CAPIO servers' memory (the check `files_on_disk` must be 0) |

Metrics (`analysis/summarize.py`):

- `prod_agg_MBps`, `cons_agg_MBps`: all bytes of that role / (last end − first
  start), over all nodes;
- for `conc`, `e2e_s` = last consumer end − first producer start, and
  `e2e_MBps` = bytes / `e2e_s`.

Page cache: in `fs-*`, a consumer on the producer's node reads from the page
cache. No root access means the cache cannot be dropped. Cross-node Lustre
reads do go through the Lustre servers.

## Test campaign

| Job | Nodes | Question | Reps |
|---|---|---|---|
| `00_smoke` | 1 | Does everything run? Kernel io_uring absent, quickstart, every config/engine/mode | 1 |
| `01_smoke_2nodes` | 2 | Do the servers connect, and does data cross nodes? | 1 |
| `10_single_node` | 1 | A: configurations; B: chunk 4K/64K/1M; C: queue depth 1–64 and `-p` | 5 / 3 / 3 |
| `11_intranode_scaling` | 1 | 1–32 concurrent pairs on one node (one CAPIO server) | 3 |
| `20_cross_node` | 2 | consumer on the other node vs local, 1 and 8 pairs per node, seq/conc | 5 |
| `30_scaling` | 1,2,4,8 | weak: 4 pairs/node; strong: 32 pairs in total | 3 |

Workload unless stated otherwise: 64 files × 8 MiB = 512 MiB per pair, chunk
256 KiB (CAPIO's cache line), queue depth 32.
