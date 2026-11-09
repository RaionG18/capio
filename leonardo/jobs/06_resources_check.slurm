#!/bin/bash
#SBATCH --job-name=capio_rescheck
#SBATCH --time=00:40:00
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=96G
#SBATCH --gres=tmpfs:20g
#
# Functional test on a shared slice (8 cores, 96 GB: a 10 GB file is held twice
# per node, and its buffer doubles by copying while it grows).
#  A. The resource measurements (client CPU and peak memory via GNU time; the
#     server sampled every second, servers.csv), at the campaign's sizes.
#  B. The paper's largest file, 1 x 10 GB, read across nodes. A remote read
#     returns the rest of the file in one message, and the MTCL backend caps a
#     message at 4 GiB: expected to fail with CAPIO's default
#     CAPIO_PREFETCH_DATA_SIZE (0 = unlimited), and to work with 1 GiB replies.
set -uo pipefail
export MAX_CONSECUTIVE_FAILS=1000 EXP_TIMEOUT=900
source "${CAPIO_REPO:-$SLURM_SUBMIT_DIR}/leonardo/lib/common.sh"
run_init resources_check

echo "==== A. measurements"
for engine in posix io_uring; do
    run_exp config=capio engine=$engine placement=cross mode=seq pairs=4 nfiles=64 fsize=8M
done
run_exp config=fs-lustre engine=posix placement=cross mode=seq pairs=4 nfiles=64 fsize=8M

echo "==== B. one 10 GB file across nodes"
run_exp config=capio engine=posix placement=cross mode=seq nfiles=1 fsize=10G chunk=1M \
    test=big_default
export CAPIO_PREFETCH_DATA_SIZE=1073741824
for engine in posix io_uring; do
    run_exp config=capio engine=$engine placement=cross mode=seq nfiles=1 fsize=10G chunk=1M \
        test=big_prefetch1G
done

run_finish
