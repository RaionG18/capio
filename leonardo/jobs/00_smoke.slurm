#!/bin/bash
#SBATCH --job-name=capio_smoke
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --gres=tmpfs:20g
#
# Functional test: a shared slice of the node (8 cores, 32 GB), not a whole node.
# Smoke test, one node: does everything work before spending node-hours?
#  1. the kernel has no io_uring (FS-uring cannot run here);
#  2. the io_uring quickstart (liburing producer/consumer, streaming) under CAPIO;
#  3. one_to_one on each configuration, engine and mode, small workload.
set -uo pipefail
source "${CAPIO_REPO:-$SLURM_SUBMIT_DIR}/leonardo/lib/common.sh"
run_init smoke

echo "== 1. kernel io_uring"
probe_dir="$DATA_ROOT/probe"
mkdir -p "$probe_dir"
"$ONE_TO_ONE" -r producer -n 1 -f 65536 -c 4096 -q 4 -e io_uring -d "$probe_dir" 2>&1 | head -1
echo "(expected: io_uring_queue_init: Function not implemented)"

echo "== 2. io_uring quickstart under CAPIO (no kernel ring)"
(CAPIO_BUILD="$CAPIO_BUILD" timeout 300 bash "$CAPIO_REPO/examples/io_uring_quickstart/run.sh") \
    >"$RUN_DIR/logs/quickstart.log" 2>&1
echo "quickstart rc=$?"
tail -3 "$RUN_DIR/logs/quickstart.log"

echo "== 3. one_to_one"
small=(nfiles=8 fsize=4M chunk=256K qd=32 rep=1)
for config in fs-lustre fs-tmp capio; do
    for engine in posix io_uring; do
        for mode in seq conc; do
            run_exp config=$config engine=$engine mode=$mode "${small[@]}"
        done
    done
done
run_exp config=capio engine=io_uring mode=seq pos=1 "${small[@]}"
run_exp config=capio engine=io_uring mode=seq pairs=4 "${small[@]}"
run_exp config=capio engine=posix mode=seq pairs=4 "${small[@]}"

run_finish
