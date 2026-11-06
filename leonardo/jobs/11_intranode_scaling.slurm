#!/bin/bash
#SBATCH --job-name=capio_intranode
#SBATCH --time=01:00:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=112
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --gres=tmpfs:100g
#
# One node, more resources: 1, 2, 4, 8, 16, 32 concurrent producer/consumer
# pairs (one CAPIO server per node serves them all). 3 reps.
# Workload per pair: 64 files x 8 MiB = 512 MiB (32 pairs = 16 GiB).
set -uo pipefail
source "${CAPIO_REPO:-$SLURM_SUBMIT_DIR}/leonardo/lib/common.sh"
run_init intranode_scaling

base=(nfiles=64 fsize=8M mode=seq)
for rep in 1 2 3; do
    for pairs in 1 2 4 8 16 32; do
        run_exp config=fs-lustre engine=posix pairs=$pairs "${base[@]}" rep=$rep
        run_exp config=fs-tmp engine=posix pairs=$pairs "${base[@]}" rep=$rep
        run_exp config=capio engine=posix pairs=$pairs "${base[@]}" rep=$rep
        run_exp config=capio engine=io_uring pairs=$pairs "${base[@]}" rep=$rep
    done
done

run_finish
