#!/bin/bash
#SBATCH --job-name=capio_smoke2
#SBATCH --time=00:20:00
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=112
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --gres=tmpfs:100g
#
# Smoke test, two nodes: CAPIO servers find each other (MTCL over TCP, discovery
# through token files on Lustre) and a consumer on node 1 reads what a producer
# on node 0 wrote, with both engines, after the producer (seq) and while it
# writes (conc).
set -uo pipefail
source "${CAPIO_REPO:-$SLURM_SUBMIT_DIR}/leonardo/lib/common.sh"
run_init smoke2

small=(nfiles=8 fsize=4M chunk=256K qd=32 rep=1)
for engine in posix io_uring; do
    for placement in local cross; do
        for mode in seq conc; do
            run_exp config=capio engine=$engine placement=$placement mode=$mode "${small[@]}"
        done
    done
done
run_exp config=fs-lustre engine=posix placement=cross mode=seq "${small[@]}"

run_finish
