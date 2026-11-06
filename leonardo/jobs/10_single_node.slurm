#!/bin/bash
#SBATCH --job-name=capio_single
#SBATCH --time=01:30:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=112
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --gres=tmpfs:100g
#
# One node, one producer/consumer pair: CAPIO-uring against CAPIO-posix and the
# filesystems.
#  A. configurations: FS-lustre, FS-tmp, CAPIO-posix, CAPIO-uring (seq), and
#     the CAPIO pair streaming (conc).                                5 reps
#  B. chunk size: 4K, 64K, 1M.                                         3 reps
#  C. io_uring queue depth: 1, 4, 16, 64; and off = -1 (-p) at 32.    3 reps
# Workload per pair: 64 files x 8 MiB = 512 MiB.
set -uo pipefail
source "${CAPIO_REPO:-$SLURM_SUBMIT_DIR}/leonardo/lib/common.sh"
run_init single_node

base=(nfiles=64 fsize=8M)

echo "==== A. configurations"
for rep in 1 2 3 4 5; do
    run_exp config=fs-lustre engine=posix mode=seq "${base[@]}" rep=$rep
    run_exp config=fs-tmp engine=posix mode=seq "${base[@]}" rep=$rep
    for engine in posix io_uring; do
        for mode in seq conc; do
            run_exp config=capio engine=$engine mode=$mode "${base[@]}" rep=$rep
        done
    done
done

echo "==== B. chunk size"
for rep in 1 2 3; do
    for chunk in 4K 64K 1M; do
        run_exp config=fs-tmp engine=posix chunk=$chunk "${base[@]}" rep=$rep
        run_exp config=capio engine=posix chunk=$chunk "${base[@]}" rep=$rep
        run_exp config=capio engine=io_uring chunk=$chunk "${base[@]}" rep=$rep
    done
done

echo "==== C. queue depth"
for rep in 1 2 3; do
    for qd in 1 4 16 64; do
        run_exp config=capio engine=io_uring qd=$qd "${base[@]}" rep=$rep
    done
    run_exp config=capio engine=io_uring qd=32 pos=1 "${base[@]}" rep=$rep
done

run_finish
