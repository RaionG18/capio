#!/bin/bash
#SBATCH --job-name=capio_stress2
#SBATCH --time=00:15:00
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --gres=tmpfs:20g
#
# Functional test: a shared slice of the node (8 cores, 32 GB), not a whole node.
# Regression check for the server-to-server deadlock (fixed in the MTCL backend):
# both nodes stream to each other at once (placement=cross, mode=conc) with
# 8 MiB files, the campaign's size, whose remote-read replies exceed the TCP
# buffers. Before the fix this hung about 1 run in 10 on a laptop. 5 reps.
set -uo pipefail
source "${CAPIO_REPO:-$SLURM_SUBMIT_DIR}/leonardo/lib/common.sh"
run_init stress2

for rep in 1 2 3 4 5; do
    for engine in io_uring posix; do
        run_exp config=capio engine=$engine placement=cross mode=conc pairs=4 \
            nfiles=16 fsize=8M rep=$rep
    done
done

run_finish
