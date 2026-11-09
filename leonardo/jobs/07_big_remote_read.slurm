#!/bin/bash
#SBATCH --job-name=capio_bigread
#SBATCH --time=00:25:00
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=96G
#SBATCH --gres=tmpfs:20g
#
# Functional test on a shared slice (8 cores, 96 GB). A remote read of a file
# larger than 4 GiB crashed the serving CAPIO server (job 63997044, e004); replies
# are now capped at the backend's maximum message. The same case with CAPIO's
# default settings, both engines, plus the campaign's sizes as a regression.
set -uo pipefail
export MAX_CONSECUTIVE_FAILS=1000 EXP_TIMEOUT=300
source "${CAPIO_REPO:-$SLURM_SUBMIT_DIR}/leonardo/lib/common.sh"
run_init big_remote_read

for engine in posix io_uring; do
    run_exp config=capio engine=$engine placement=cross mode=seq nfiles=1 fsize=10G chunk=1M
done
for engine in posix io_uring; do
    run_exp config=capio engine=$engine placement=cross mode=seq pairs=4 nfiles=64 fsize=8M
    run_exp config=capio engine=$engine placement=cross mode=conc pairs=4 nfiles=64 fsize=8M
done
run_finish
