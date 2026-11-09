#!/bin/bash
#SBATCH --job-name=capio_seek
#SBATCH --time=00:30:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --gres=tmpfs:20g
#
# Shared slice (8 cores, 32 GB): one pair needs 2 cores. Tests the thesis'
# hypothesis for the io_uring cost (RQ2): every positioned SQE makes the client
# flush and seek through the server (two capio_lseek round trips). With -p
# (off = -1, current position) there is no seek. Job 10 measured -p only at
# 256 KiB (820 -> 898 MB/s read); the gap is largest at 4 KiB (76 vs 946).
# Baselines in the same job, so the comparisons are within one allocation.
set -uo pipefail
source "${CAPIO_REPO:-$SLURM_SUBMIT_DIR}/leonardo/lib/common.sh"
run_init seek_test

base=(nfiles=64 fsize=8M mode=seq)
for rep in 1 2 3 4 5; do
    for chunk in 4K 64K 256K; do
        run_exp config=capio engine=posix chunk=$chunk "${base[@]}" rep=$rep
        for pos in 0 1; do
            run_exp config=capio engine=io_uring chunk=$chunk pos=$pos "${base[@]}" rep=$rep
        done
    done
done
run_finish
