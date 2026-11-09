#!/bin/bash
#SBATCH --job-name=capio_logs
#SBATCH --time=00:30:00
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --gres=tmpfs:20g
#
# Trace run: CAPIO's internal logs (every request, in the server and in each
# client, per thread) for the cases the campaign measured, with small files.
# Uses the log build (./leonardo/build.sh --logs, a Debug build), so the timings
# of this job are NOT measurements: it records what CAPIO does, not how fast.
# Shared slice (8 cores, 32 GB). Each experiment's logs end up packed in
# logs/<exp>/node<k>/capio_logs.tar.gz.
#   - chunk 4K vs 256K, one pair (io_uring is 12x slower than POSIX at 4K)
#   - streaming on one node and across nodes (remote reads)
#   - 4 pairs on one node (server contention)
set -uo pipefail
export CAPIO_BUILD="${CAPIO_REPO:-$SLURM_SUBMIT_DIR}/build-leonardo-log"
export CAPIO_LOGS=1
source "${CAPIO_REPO:-$SLURM_SUBMIT_DIR}/leonardo/lib/common.sh"
[ -x "$CAPIO_SERVER" ] || { echo "no log build in $CAPIO_BUILD: run ./leonardo/build.sh --logs"; exit 1; }
run_init capio_logs

small=(nfiles=4 fsize=1M)
for engine in io_uring posix; do
    for chunk in 4K 256K; do
        run_exp config=capio engine=$engine mode=seq chunk=$chunk nodes=1 "${small[@]}"
    done
    run_exp config=capio engine=$engine mode=conc nodes=1 "${small[@]}"
    run_exp config=capio engine=$engine placement=cross mode=conc nodes=2 "${small[@]}"
    run_exp config=capio engine=$engine mode=seq pairs=4 nodes=1 "${small[@]}"
done

run_finish
