#!/bin/bash
#SBATCH --job-name=capio_scaling
#SBATCH --time=01:00:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=112
#SBATCH --exclusive
#SBATCH --mem=0
#SBATCH --gres=tmpfs:100g
#
# Multi-node scaling. Submit once per node count:
#   for n in 1 2 4 8; do ./leonardo/submit.sh leonardo/jobs/30_scaling.slurm --nodes=$n; done
#
# Consumers run on the next node (placement=cross), so data moves between nodes.
#  weak:   4 pairs per node (total data grows with the nodes), seq and conc.
#  strong: 32 pairs in total, 32/N per node (total data fixed at 16 GiB), seq.
# Workload per pair: 64 files x 8 MiB = 512 MiB. 3 reps.
set -uo pipefail
source "${CAPIO_REPO:-$SLURM_SUBMIT_DIR}/leonardo/lib/common.sh"
run_init "scaling_n$SLURM_JOB_NUM_NODES"

base=(nfiles=64 fsize=8M placement=cross)
strong_pairs=$((32 / NNODES))
for rep in 1 2 3; do
    # weak
    w=(test=weak "${base[@]}")
    run_exp config=fs-lustre engine=posix mode=seq pairs=4 "${w[@]}" rep=$rep
    run_exp config=fs-tmp engine=posix mode=seq pairs=4 "${w[@]}" placement=local rep=$rep
    for engine in posix io_uring; do
        for mode in seq conc; do
            run_exp config=capio engine=$engine mode=$mode pairs=4 "${w[@]}" rep=$rep
        done
    done
    # strong
    st=(test=strong "${base[@]}")
    run_exp config=fs-lustre engine=posix mode=seq pairs=$strong_pairs "${st[@]}" rep=$rep
    for engine in posix io_uring; do
        run_exp config=capio engine=$engine mode=seq pairs=$strong_pairs "${st[@]}" rep=$rep
    done
done

run_finish
