#!/bin/bash
#SBATCH --job-name=capio_replysize
#SBATCH --time=00:40:00
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=96G
#SBATCH --gres=tmpfs:20g
#
# Shared slice (8 cores, 96 GB). One 10 GB file read across nodes, with remote
# read replies of 256 MiB, 1 GiB and 4 GiB (CAPIO_PREFETCH_DATA_SIZE). Replies
# of 4 GiB kept the receiving server at 100% CPU for minutes (job 63999935);
# this picks the largest reply size that is still efficient.
set -uo pipefail
export MAX_CONSECUTIVE_FAILS=1000 EXP_TIMEOUT=300
source "${CAPIO_REPO:-$SLURM_SUBMIT_DIR}/leonardo/lib/common.sh"
run_init reply_size

for size in 268435456 1073741824 4294967296; do
    export CAPIO_PREFETCH_DATA_SIZE=$size
    for engine in posix io_uring; do
        run_exp config=capio engine=$engine placement=cross mode=seq nfiles=1 fsize=10G chunk=1M \
            test=reply_$((size >> 20))MiB
    done
done
run_finish
