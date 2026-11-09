#!/bin/bash
#SBATCH --job-name=capio_ibcheck
#SBATCH --time=00:20:00
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --gres=tmpfs:20g
#
# Functional test: a shared slice of the node (8 cores, 32 GB), not a whole node.
# CAPIO server traffic over InfiniBand (CAPIO_MTCL_ADVERTISE_HOST, set by
# node_run) instead of the 1 Gb/s management network the hostname resolves to.
# The cross-node shapes of job 20 at the campaign's sizes (64 files x 8 MiB per
# pair); over the management network these read at 27 MB/s per pair (1 pair)
# and 228 MB/s in total (8 pairs), job 63952928.
set -uo pipefail
source "${CAPIO_REPO:-$SLURM_SUBMIT_DIR}/leonardo/lib/common.sh"
run_init ib_check

base=(nfiles=64 fsize=8M placement=cross rep=1)
for engine in io_uring posix; do
    run_exp config=capio engine=$engine mode=seq pairs=1 "${base[@]}"
    run_exp config=capio engine=$engine mode=seq pairs=8 "${base[@]}"
    run_exp config=capio engine=$engine mode=conc pairs=8 "${base[@]}"
done
run_finish
