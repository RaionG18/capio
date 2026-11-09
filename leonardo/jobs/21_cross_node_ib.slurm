#!/bin/bash
#SBATCH --job-name=capio_cross_ib
#SBATCH --time=00:40:00
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=112
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --gres=tmpfs:100g
#
# Rerun of the cross-node half of job 20 (63952928), whose CAPIO servers talked
# over the 1 Gb/s management network: now over InfiniBand (node_run sets
# CAPIO_MTCL_ADVERTISE_HOST). Same experiments, parameters and reps; the Lustre
# baseline is repeated so both come from the same session. Job 20's
# placement=local results stay valid (no server-to-server traffic).
set -uo pipefail
source "${CAPIO_REPO:-$SLURM_SUBMIT_DIR}/leonardo/lib/common.sh"
run_init cross_node_ib

base=(nfiles=64 fsize=8M)
for rep in 1 2 3 4 5; do
    for pairs in 1 8; do
        run_exp config=fs-lustre engine=posix placement=cross mode=seq pairs=$pairs "${base[@]}" rep=$rep
        for engine in posix io_uring; do
            for mode in seq conc; do
                run_exp config=capio engine=$engine placement=cross mode=$mode \
                    pairs=$pairs "${base[@]}" rep=$rep
            done
        done
    done
done

run_finish
