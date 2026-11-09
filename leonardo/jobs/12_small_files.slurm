#!/bin/bash
#SBATCH --job-name=capio_smallfiles
#SBATCH --time=02:00:00
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --gres=tmpfs:20g
#
# Many small files (4 KiB), consumer on the other node. Shared slices (8 cores
# per node): CAPIO's per-file cost grows with the number of files (every create
# and lookup re-reads all nodes' location files; pilot 64001589, laptop test),
# a 15-40x effect, far above the noise of neighbouring jobs.
#  A. files per pair: 250 .. 4000, 1 pair. Streaming only up to 1000 files.
#  B. pairs per node: 1, 2, 4, 8 with 250 files each.
# CAPIO (both engines) and Lustre; 3 reps; resource figures in servers.csv.
set -uo pipefail
export EXP_TIMEOUT=900
source "${CAPIO_REPO:-$SLURM_SUBMIT_DIR}/leonardo/lib/common.sh"
run_init small_files

small=(fsize=4K chunk=4K placement=cross)
for rep in 1 2 3; do
    for nf in 250 500 1000 2000 4000; do
        run_exp config=fs-lustre engine=posix mode=seq nfiles=$nf pairs=1 "${small[@]}" \
            test=files_per_pair rep=$rep
        for engine in posix io_uring; do
            run_exp config=capio engine=$engine mode=seq nfiles=$nf pairs=1 "${small[@]}" \
                test=files_per_pair rep=$rep
            [ "$nf" -le 1000 ] && run_exp config=capio engine=$engine mode=conc nfiles=$nf \
                pairs=1 "${small[@]}" test=files_per_pair rep=$rep
        done
    done
    for pairs in 1 2 4 8; do
        run_exp config=fs-lustre engine=posix mode=seq nfiles=250 pairs=$pairs "${small[@]}" \
            test=pairs rep=$rep
        for engine in posix io_uring; do
            run_exp config=capio engine=$engine mode=seq nfiles=250 pairs=$pairs "${small[@]}" \
                test=pairs rep=$rep
        done
    done
done
run_finish
