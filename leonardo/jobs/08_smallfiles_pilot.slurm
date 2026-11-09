#!/bin/bash
#SBATCH --job-name=capio_smallpilot
#SBATCH --time=00:45:00
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --gres=tmpfs:20g
#
# Pilot on a shared slice (8 cores, 32 GB): many small files (4 KiB), where
# Lustre's metadata server should dominate, and where CAPIO records the location
# of every new file in a file on Lustre under a lock. Measures files per second
# for 1,000 and 10,000 files per pair, 1 and 8 pairs, to size the real study.
# Time limits are not data-sized here (4 KiB files move little data).
set -uo pipefail
export EXP_TIMEOUT=600
source "${CAPIO_REPO:-$SLURM_SUBMIT_DIR}/leonardo/lib/common.sh"
run_init smallfiles_pilot

for nf in 1000 10000; do
    for pairs in 1 8; do
        small=(nfiles=$nf fsize=4K chunk=4K pairs=$pairs)
        run_exp config=fs-tmp engine=posix placement=local mode=seq nodes=1 "${small[@]}"
        run_exp config=fs-lustre engine=posix placement=cross mode=seq "${small[@]}"
        for engine in posix io_uring; do
            for mode in seq conc; do
                run_exp config=capio engine=$engine placement=cross mode=$mode "${small[@]}"
            done
        done
    done
done
run_finish
