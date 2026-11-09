#!/bin/bash
#SBATCH --job-name=capio_paper1k
#SBATCH --time=03:00:00
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=64G
#SBATCH --gres=tmpfs:20g
#
# The CAPIO paper's 1 KiB calls, on 100 x 100 MiB per pair, consumer on the
# other node, write-then-read. io_uring with seeks (pos=0) and without (pos=1,
# off = -1). From the 4 KiB rates (seek test 63995995), io_uring with seeks
# should need ~15-20 min per experiment, so it gets 2 reps and a 30 min limit;
# a timeout there is itself a result (rate below 10 GiB / 30 min per node).
set -uo pipefail
export EXP_TIMEOUT=1800
source "${CAPIO_REPO:-$SLURM_SUBMIT_DIR}/leonardo/lib/common.sh"
run_init paper_sizes_1k

p=(nodes=2 placement=cross mode=seq pairs=1 nfiles=100 fsize=100M chunk=1K)
for rep in 1 2 3; do
    run_exp config=fs-lustre engine=posix "${p[@]}" rep=$rep
    run_exp config=capio engine=posix "${p[@]}" rep=$rep
    run_exp config=capio engine=io_uring pos=1 "${p[@]}" rep=$rep
    [ "$rep" -le 2 ] && run_exp config=capio engine=io_uring pos=0 "${p[@]}" rep=$rep
done
run_finish
