#!/bin/bash
#SBATCH --job-name=capio_cross
#SBATCH --time=01:00:00
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=112
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --gres=tmpfs:100g
#
# Two nodes, distributed pipeline: the consumer of pair g runs on the other node,
# so every byte crosses the network (CAPIO: server to server over MTCL/TCP;
# FS-lustre: through the Lustre servers). Compared with the same pairs placed
# locally. 1 and 8 pairs per node; seq and conc (streaming). 5 reps.
# Workload per pair: 64 files x 8 MiB = 512 MiB.
set -uo pipefail
source "${CAPIO_REPO:-$SLURM_SUBMIT_DIR}/leonardo/lib/common.sh"
run_init cross_node

base=(nfiles=64 fsize=8M)
for rep in 1 2 3 4 5; do
    for pairs in 1 8; do
        run_exp config=fs-lustre engine=posix placement=cross mode=seq pairs=$pairs "${base[@]}" rep=$rep
        for engine in posix io_uring; do
            for placement in local cross; do
                for mode in seq conc; do
                    run_exp config=capio engine=$engine placement=$placement mode=$mode \
                        pairs=$pairs "${base[@]}" rep=$rep
                done
            done
        done
    done
done

run_finish
