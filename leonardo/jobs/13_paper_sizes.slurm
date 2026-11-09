#!/bin/bash
#SBATCH --job-name=capio_paper
#SBATCH --time=05:00:00
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=64G
#SBATCH --gres=tmpfs:20g
#
# The CAPIO paper's file sizes, 10 GiB per pair: 100 x 100 MiB, 10 x 1 GiB,
# 1 x 10 GiB, written and read in 1 MiB calls (1 KiB calls: job 14).
#   1 node:  consumer on the same node; Lustre, node-local tmpfs, CAPIO.
#   2 nodes: consumer on the other node (both nodes produce: 20 GiB per server).
# CAPIO with both engines, write-then-read and streaming; 5 reps, reps outermost.
# Shared slices (8 cores, 64 GB per node): one pair per node needs 2 cores; the
# server peaks at ~1.4x the data it holds (28 GB, job 63997044). Remote replies
# are capped at 4 GiB since 150f074.
set -uo pipefail
source "${CAPIO_REPO:-$SLURM_SUBMIT_DIR}/leonardo/lib/common.sh"
run_init paper_sizes

sizes=("100 100M" "10 1G" "1 10G")
for rep in 1 2 3 4 5; do
    for s in "${sizes[@]}"; do
        read -r nf fs <<<"$s"
        p=(pairs=1 nfiles=$nf fsize=$fs chunk=1M rep=$rep)
        run_exp config=fs-lustre engine=posix nodes=1 placement=local mode=seq "${p[@]}"
        run_exp config=fs-tmp engine=posix nodes=1 placement=local mode=seq "${p[@]}"
        run_exp config=fs-lustre engine=posix nodes=2 placement=cross mode=seq "${p[@]}"
        for engine in posix io_uring; do
            for mode in seq conc; do
                run_exp config=capio engine=$engine nodes=1 placement=local mode=$mode "${p[@]}"
                run_exp config=capio engine=$engine nodes=2 placement=cross mode=$mode "${p[@]}"
            done
        done
    done
done
run_finish
